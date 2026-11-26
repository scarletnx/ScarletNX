/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The T16 and A32 emitters tools/sailgen generates from the Sail (src/common/cpu/arm/sail/, its back end llvm_arm.h)
   against the ones written by hand they replaced (llvm_thumb.inc, snx_thumb_llvm_block_t16_reference), halfword by
   halfword: the same IR, and the same results.
     snx-test-arm-sail-ir [--a32] [--report FILE] [--states N] [--part K/N] [--every N] [--time ROUNDS]
   Every T16 halfword (0x0000 to 0xE7FF, and to 0xFFFF on a profile with no Thumb-2, whose BL halves are 16-bit
   instructions), on every profile and the memory modes it takes (the A57's, the A9's and the ARM11's flat, checked and
   hosted, the ARM7TDMI's flat with its bus trace and without, the ARM946E-S's flat), alone
   in its block and, where the profile has IT, the last of an IT block (IT GT) and the first of two (ITT GT, a MOV
   after it), becomes a block function in two modules, one emitted by each, optimized at O2 as the executor's objects
   are (snx_ir_module_optimize), and the two functions are compared: the same text, the same instructions in another
   order, or another IR no longer than the hand-written one; a longer one fails. Then the flat blocks run on this host
   from random states (registers pointing into 64 KiB of guest memory of random bytes, or anywhere; NZCV random), the
   ARM7TDMI's half of the time with a bus hook that answers every access it routes and logs it, and the registers,
   CPSR, the exit and its syndrome, the exclusive monitor, the trace's slots, guest memory and the hook's log must
   match. --a32 holds the A32 emitters to llvm.c's (snx_arm_llvm_block_reference) the same way, each of 65536 words of
   the subsets the Sail's take (a32_word) alone in its block. Before the T16 halfwords, the witnesses: blocks whose
   answer the architecture gives (BKPT in an IT block, the UNPREDICTABLE forms run as written), run with both
   emitters. --part K/N and --every N take the Kth of N stretches, 1 in --every of them all, of a walk that visits
   each once (a ctest part's share, spread over every register and immediate); a sample of 512 or more that misses a
   value of a register field fails; --report writes each
   longer block's two IR texts; --time ROUNDS times the two emitters over the halfwords (time_emitters) */
#include "common/cpu/arm/llvm.h"
#include "common/cpu/arm/simd.h"
#include "common/cpu/recompiler/loader/code_loader.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/utilities/common.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;

/* a profile and memory mode the halfwords are emitted for, and whether its blocks run here */
typedef struct {
    const char* name;
    SnxArmProfile profile;
    SnxArmMemoryMode memory;
    int it;    /* the profile has IT (Thumb-2): its halfwords also run in an IT block */
    int trace; /* the ARM7TDMI's trace of addresses and multipliers (SnxArmCodegenOptions.trace) */
    int run;   /* flat: run here */
} Config;
static const Config configs[] = {
    {"a57", SNX_ARM_PROFILE_A57, SNX_ARM_MEMORY_FLAT, 1, 0, 1},
    {"a57-checked", SNX_ARM_PROFILE_A57, SNX_ARM_MEMORY_CHECKED, 1, 0, 0},
    {"a57-hosted", SNX_ARM_PROFILE_A57, SNX_ARM_MEMORY_HOSTED, 1, 0, 0},
    {"a9", SNX_ARM_PROFILE_A9, SNX_ARM_MEMORY_FLAT, 1, 0, 1},
    {"a9-hosted", SNX_ARM_PROFILE_A9, SNX_ARM_MEMORY_HOSTED, 1, 0, 0},
    {"arm11", SNX_ARM_PROFILE_ARM11, SNX_ARM_MEMORY_FLAT, 0, 0, 1},
    {"arm11-checked", SNX_ARM_PROFILE_ARM11, SNX_ARM_MEMORY_CHECKED, 0, 0, 0},
    {"arm11-hosted", SNX_ARM_PROFILE_ARM11, SNX_ARM_MEMORY_HOSTED, 0, 0, 0},
    {"arm7tdmi", SNX_ARM_PROFILE_ARM7TDMI, SNX_ARM_MEMORY_FLAT, 0, 0, 1},
    {"arm7tdmi-trace", SNX_ARM_PROFILE_ARM7TDMI, SNX_ARM_MEMORY_FLAT, 0, 1, 1},
    {"arm946e", SNX_ARM_PROFILE_ARM946E, SNX_ARM_MEMORY_FLAT, 0, 0, 1},
};
/* where the halfword is in its block: alone; the last of an IT block (IT GT before it); the first of two (ITT GT
   before it, MOV r0, r0 after it) */
enum { ALONE, IT_LAST, IT_FIRST, PLACES };
static const char* const places[PLACES] = {"alone", "last in IT", "first in IT"};

#define PC        0x1000u /* the block's address, in guest memory */
#define MEM_SIZE  0x10000u
#define TRACE     ((uint32_t)sizeof(SnxArmCPU)) /* the trace's slots, after the CPU structure */
#define SKIPPED   (TRACE + 8 * 4)               /* and its skipped bits */
#define CPU_BYTES (SKIPPED + 64)
#define HOOK_LOG  64

static struct {
    FILE* report;
    unsigned states, part, parts, every, time_rounds;
    size_t failures, compared, ran, outcomes[4];
    u64 seed;
    /* the bus hook's log of the block running (hook) */
    uint32_t log[HOOK_LOG][3];
    unsigned logged;
    char why[160]; /* what differs between two runs (differs) */
    int a32;       /* --a32: A32 words, not T16 halfwords */
    int only;      /* --word HEX: that halfword or word alone, each run's difference said */
    uint32_t word;
} g_test;

static u64 rnd(void) {
    u64 x = g_test.seed += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/* ---- the same instructions in another order (llvm_sail_ir.c's: an instruction's hash its opcode, type, predicate,
   ordering and its operands' hashes, sorted for the commutative ones) ---- */
typedef struct {
    LLVMValueRef v;
    u64 h;
} Memo;
typedef struct {
    Memo* memo;
    size_t count, cap;
    LLVMValueRef fn;
} Dag;
static u64 fnv(u64 h, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static u64 fnv_text(u64 h, char* text) {
    h = fnv(h, text, strlen(text));
    LLVMDisposeMessage(text);
    return h;
}
static int cmp_u32(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}
/* whether a sample of 512 or more holds every value of its register fields (a T16 instruction's Rd, Rn or Rm at bits
   0, 3 and 6, the bits most forms take them in; an A32 one's Rm, Rd and Rn at 0, 12 and 16): one that misses one
   tests a share of the registers only */
static int sample_spread(const uint32_t* items, size_t count) {
    static const unsigned t16[3] = {0, 3, 6}, a32[3] = {0, 12, 16};
    unsigned bits = g_test.a32 ? 4 : 3;
    if (count < 512) return 0;
    for (unsigned f = 0; f < 3; ++f) {
        unsigned at = g_test.a32 ? a32[f] : t16[f], seen = 0, values = 0;
        for (size_t i = 0; i < count; ++i) seen |= 1u << ((items[i] >> at) & ((1u << bits) - 1));
        for (unsigned v = 0; v < 1u << bits; ++v) values += (seen >> v) & 1;
        if (values != 1u << bits) {
            fprintf(stderr, "FAIL: the %zu %s sampled hold %u of the %u values of the field at bit %u (%#06x)\n", count,
                    g_test.a32 ? "words" : "halfwords", values, 1u << bits, at, seen);
            return 1;
        }
    }
    return 0;
}
static int cmp_u64(const void* a, const void* b) {
    u64 x = *(const u64*)a, y = *(const u64*)b;
    return x < y ? -1 : x > y;
}
static u64 value_hash(Dag* d, LLVMValueRef v) {
    u64 h = 0xcbf29ce484222325ull;
    if (LLVMIsAInstruction(v)) {
        LLVMOpcode op = LLVMGetInstructionOpcode(v);
        unsigned n = (unsigned)LLVMGetNumOperands(v), i;
        u64 ops[16];
        for (i = 0; i < d->count; ++i)
            if (d->memo[i].v == v) return d->memo[i].h;
        h = fnv(h, &op, sizeof op);
        h = fnv_text(h, LLVMPrintTypeToString(LLVMTypeOf(v)));
        if (op == LLVMICmp) {
            LLVMIntPredicate p = LLVMGetICmpPredicate(v);
            h = fnv(h, &p, sizeof p);
        }
        if (op == LLVMFence || op == LLVMLoad || op == LLVMStore || op == LLVMAtomicRMW) {
            LLVMAtomicOrdering o = LLVMGetOrdering(v);
            h = fnv(h, &o, sizeof o);
        }
        if (n > 16) n = 16;
        for (i = 0; i < n; ++i) ops[i] = value_hash(d, LLVMGetOperand(v, i));
        if (op == LLVMAdd || op == LLVMMul || op == LLVMAnd || op == LLVMOr || op == LLVMXor ||
            (op == LLVMICmp && (LLVMGetICmpPredicate(v) == LLVMIntEQ || LLVMGetICmpPredicate(v) == LLVMIntNE)))
            qsort(ops, n, sizeof *ops, cmp_u64);
        h = fnv(h, ops, n * sizeof *ops);
        if (d->count == d->cap) {
            d->cap = d->cap ? d->cap * 2 : 64;
            d->memo = (Memo*)snx_realloc(d->memo, d->cap * sizeof *d->memo);
        }
        d->memo[d->count].v = v;
        d->memo[d->count].h = h;
        ++d->count;
        return h;
    }
    if (LLVMValueIsBasicBlock(v)) {
        unsigned k = 0;
        for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(d->fn); b; b = LLVMGetNextBasicBlock(b), ++k)
            if (LLVMBasicBlockAsValue(b) == v) return fnv(h ^ 0xb10c, &k, sizeof k);
        return h;
    }
    if (LLVMIsAArgument(v)) {
        unsigned k = 0;
        for (LLVMValueRef a = LLVMGetFirstParam(d->fn); a; a = LLVMGetNextParam(a), ++k)
            if (a == v) return fnv(h ^ 0xa49, &k, sizeof k);
        return h;
    }
    if (LLVMIsAGlobalValue(v)) {
        size_t length;
        const char* name = LLVMGetValueName2(v, &length);
        return fnv(h ^ 0x610b, name, length);
    }
    return fnv_text(h, LLVMPrintValueToString(v));
}
static u64* dag(LLVMValueRef fn, size_t* count) {
    Dag d = {NULL, 0, 0, fn};
    size_t n = 0, cap = 64;
    u64* list = (u64*)snx_realloc(NULL, cap * sizeof *list);
    for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(fn); b; b = LLVMGetNextBasicBlock(b))
        for (LLVMValueRef i = LLVMGetFirstInstruction(b); i; i = LLVMGetNextInstruction(i)) {
            if (n == cap) list = (u64*)snx_realloc(list, (cap *= 2) * sizeof *list);
            list[n++] = value_hash(&d, i);
        }
    qsort(list, n, sizeof *list, cmp_u64);
    free(d.memo);
    *count = n;
    return list;
}
/* the function's body, its name and metadata left out */
static char* body_text(LLVMValueRef fn) {
    char* text = LLVMPrintValueToString(fn);
    const char* s = strchr(text, '{');
    char* out = snx_strdup(s ? s : text);
    LLVMDisposeMessage(text);
    return out;
}
enum { SAME, REORDERED, OTHER_NOT_LONGER, LONGER };
static const char* const outcome[4] = {"the same", "the same instructions reordered", "another IR, not longer",
                                       "LONGER"};
static int compare_fns(LLVMValueRef hand, LLVMValueRef gen, char** text_hand, char** text_gen, size_t* sizes) {
    size_t n0, n1;
    u64 *a = dag(hand, &n0), *b = dag(gen, &n1);
    int r;
    *text_hand = body_text(hand);
    *text_gen = body_text(gen);
    sizes[0] = n0;
    sizes[1] = n1;
    if (!strcmp(*text_hand, *text_gen))
        r = SAME;
    else if (n0 == n1 && !memcmp(a, b, n0 * sizeof *a))
        r = REORDERED;
    else
        r = n1 > n0 ? LONGER : OTHER_NOT_LONGER;
    free(a);
    free(b);
    return r;
}

/* ---- the blocks ---- */

/* the halfwords of the block that tests h at place: their count */
static size_t block_of(uint32_t h, int place, uint16_t out[3]) {
    if (g_test.a32) { /* an A32 word, alone: its halfwords in memory's order */
        out[0] = (uint16_t)h;
        out[1] = (uint16_t)(h >> 16);
        return 2;
    }
    if (place == ALONE) {
        out[0] = h;
        return 1;
    }
    if (place == IT_LAST) {
        out[0] = 0xBFC8; /* IT GT */
        out[1] = h;
        return 2;
    }
    out[0] = 0xBFC4; /* ITT GT */
    out[1] = h;
    out[2] = 0x4600; /* MOV r0, r0 */
    return 3;
}
static SnxArmCodegenOptions options_of(const Config* c) {
    SnxArmCodegenOptions o = {SNX_ARM_CODEGEN_OPTIONS_ABI, c->profile, c->memory, 0, 0};
    if (c->trace) {
        o.trace = TRACE;
        o.skipped = SKIPPED;
    }
    return o;
}
/* one module's blocks, from one emitter: the functions in fn, NULL for a block it refused */
static LLVMModuleRef emit_all(SnxIr* ir, const Config* c, int place, const uint32_t* halfwords, size_t count,
                              int reference, LLVMValueRef* fn) {
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext(reference ? "hand" : "sail", ir->ctx);
    SnxArmCodegenOptions o = options_of(c);
    LLVMSetTarget(module, LLVMGetTarget(ir->mod));
    LLVMSetDataLayout(module, LLVMGetDataLayoutStr(ir->mod));
    for (size_t i = 0; i < count; ++i) {
        uint16_t code[3];
        size_t n = block_of(halfwords[i], place, code), consumed;
        char name[32];
        snprintf(name, sizeof name, "T_%zu", i);
        if (g_test.a32) {
            uint32_t word = halfwords[i];
            fn[i] = reference ? snx_arm_llvm_block_reference(module, name, &word, 1, PC, &consumed, &o)
                              : snx_arm_llvm_block_ex(module, name, &word, 1, PC, &consumed, &o);
        } else
            fn[i] = reference ? snx_thumb_llvm_block_t16_reference(module, name, code, n, PC, &consumed, &o)
                              : snx_thumb_llvm_block_ex(module, name, code, n, PC, &consumed, &o);
    }
    return module;
}

/* ---- running ---- */

/* the ARM7TDMI's and the ARM946E-S's bus hook: an answer made from what it is asked, logged, of the width read as the
   boards' hooks answer (arm.h): zero extended, or for the ARM7TDMI's LDRSH (0x200) its halfword, or at an odd address
   its byte, sign extended */
static uint32_t hook(SnxArmCPU* cpu, uint32_t address, uint32_t size_write, uint32_t value) {
    uint32_t v = (address * 0x9E3779B1u) ^ size_write, bytes = size_write & 0xFF;
    (void)cpu;
    if (g_test.logged < HOOK_LOG) {
        g_test.log[g_test.logged][0] = address;
        g_test.log[g_test.logged][1] = size_write;
        g_test.log[g_test.logged][2] = value;
    }
    ++g_test.logged;
    if (size_write & 0x100) return 0;
    if (size_write & 0x200) return address & 1 ? (uint32_t)(int32_t)(int8_t)v : (uint32_t)(int32_t)(int16_t)v;
    return bytes >= 4 ? v : v & ((1u << bytes * 8) - 1);
}
typedef struct {
    union { /* the CPU structure, the block's trace after it: aligned as the structure, in each RunState of an array */
        SnxArmCPU regs;
        unsigned char bytes[CPU_BYTES];
    } cpu;
    unsigned char mem[MEM_SIZE];
    uint32_t log[HOOK_LOG][3];
    unsigned logged;
} RunState;
static_assert(sizeof(RunState) % alignof(SnxArmCPU) == 0, "the second RunState of two must hold an aligned SnxArmCPU");
/* a random state: registers into guest memory or anywhere (all of them, the even or the odd ones, by state: a negative
   operand, a shift by 32 or more), NZCV random, Thumb, user mode */
static void start(RunState* s, unsigned k, const uint16_t* code, size_t n, int legacy) {
    SnxArmCPU* cpu = &s->cpu.regs;
    memset(&s->cpu, 0, sizeof s->cpu);
    for (size_t i = 0; i < MEM_SIZE; i += 8) {
        u64 x = rnd();
        memcpy(s->mem + i, &x, 8);
    }
    for (size_t i = 0; i < n; ++i) memcpy(s->mem + PC + 2 * i, &code[i], 2); /* little-endian host */
    for (unsigned r = 0; r < 15; ++r) {
        u64 x = rnd();
        int anywhere = k % 4 == 3 || (k % 4 == 2 && (r & 1)) || (k % 4 == 0 && !(r & 1));
        cpu->r[r] = anywhere ? (uint32_t)x : (uint32_t)(x % (MEM_SIZE - 0x400)) & (k % 4 == 1 ? ~3u : ~0u);
    }
    cpu->r[13] = 0x8000u + (uint32_t)(rnd() % 0x4000) * (k % 2 ? 4 : 1);
    cpu->r[15] = PC;
    cpu->cpsr = 0x10u | (g_test.a32 ? 0 : SNX_ARM_T) | ((uint32_t)rnd() & 0xF0000000u) | (k % 3 == 0 ? 0x08000000u : 0);
    cpu->mem = s->mem;
    cpu->mem_size = MEM_SIZE;
    cpu->io = legacy && k % 2 ? hook : NULL;
    cpu->exclusive_valid = k % 5 == 0;
    cpu->exclusive_address = cpu->r[(k / 5) % 8];
    cpu->exclusive_size = 4;
}
static void finish(RunState* s) {
    SnxArmCPU* cpu = &s->cpu.regs;
    memcpy(s->log, g_test.log, sizeof s->log);
    s->logged = g_test.logged;
    cpu->mem = NULL;
    cpu->io = NULL;
}
/* what differs between the two runs, "" for nothing */
static const char* differs(const RunState* a, const RunState* b) {
    char* why = g_test.why;
    const SnxArmCPU *x = &a->cpu.regs, *y = &b->cpu.regs;
    for (unsigned r = 0; r < 16; ++r)
        if (x->r[r] != y->r[r]) {
            snprintf(why, sizeof g_test.why, "r%u %08x, generated %08x", r, x->r[r], y->r[r]);
            return why;
        }
    if (x->cpsr != y->cpsr) return snprintf(why, sizeof g_test.why, "cpsr %08x, generated %08x", x->cpsr, y->cpsr), why;
    if (x->exit != y->exit) return snprintf(why, sizeof g_test.why, "exit %u, generated %u", x->exit, y->exit), why;
    if (x->syndrome != y->syndrome)
        return snprintf(why, sizeof g_test.why, "syndrome %08x, generated %08x", x->syndrome, y->syndrome), why;
    if (x->exclusive_valid != y->exclusive_valid)
        return snprintf(why, sizeof g_test.why, "exclusive_valid %u, generated %u", x->exclusive_valid,
                        y->exclusive_valid),
               why;
    if (memcmp(a->cpu.bytes + TRACE, b->cpu.bytes + TRACE, CPU_BYTES - TRACE)) return "the trace";
    if (memcmp(a->mem, b->mem, MEM_SIZE)) return "guest memory";
    if (a->logged != b->logged)
        return snprintf(why, sizeof g_test.why, "%u bus hook calls, generated %u", a->logged, b->logged), why;
    for (unsigned i = 0; i < a->logged && i < HOOK_LOG; ++i)
        if (memcmp(a->log[i], b->log[i], sizeof a->log[i])) {
            snprintf(why, sizeof g_test.why, "bus hook call %u: %08x %x %08x, generated %08x %x %08x", i, a->log[i][0],
                     a->log[i][1], a->log[i][2], b->log[i][0], b->log[i][1], b->log[i][2]);
            return why;
        }
    return "";
}

/* the two modules' objects loaded, the blocks' addresses in addr[2][count] */
static int load(SnxIr* ir, SnxCodeLoader* loader, LLVMModuleRef module[2], size_t count, SnxU64* addr[2]) {
    char error[512];
    for (int k = 0; k < 2; ++k) {
        unsigned char* object = NULL;
        size_t size = 0;
        SnxLinkInput input = {0};
        const char** names = (const char**)snx_realloc(NULL, sizeof(char*) * (count + 1));
        char (*storage)[32] = snx_realloc(NULL, 32 * (count + 1));
        if (snx_ir_module_object(ir, module[k], '2', &object, &size, error, sizeof error)) {
            fprintf(stderr, "FAIL: %s object: %s\n", k ? "generated" : "hand-written", error);
            free(names);
            free(storage);
            return -1;
        }
        for (size_t i = 0; i < count; ++i) {
            snprintf(storage[i], sizeof storage[i], "T_%zu", i);
            names[i] = storage[i];
        }
        input.data = object;
        input.size = size;
        input.name = k ? "sail" : "hand";
        if (snx_code_load_private(loader, &input, 1, names, count, addr[k], error, sizeof error)) {
            fprintf(stderr, "FAIL: %s load: %s\n", k ? "generated" : "hand-written", error);
            free(object);
            free(names);
            free(storage);
            return -1;
        }
        free(object);
        free(names);
        free(storage);
    }
    return 0;
}

/* ---- one configuration ---- */

static void judge(int r, const Config* c, int place, uint32_t h, const char* a, const char* b, const size_t* sizes) {
    ++g_test.outcomes[r];
    ++g_test.compared;
    if (r == LONGER) {
        ++g_test.failures;
        fprintf(stderr, "FAIL: %04x %s (%s): the generated IR has %zu instructions, the hand-written %zu\n", h, c->name,
                places[place], sizes[1], sizes[0]);
    }
    if ((r == LONGER || g_test.only) && g_test.report) /* every halfword's IR in every place would be gigabytes */
        fprintf(g_test.report, "== %04x %s %s: %s (%zu, %zu)\n-- hand-written\n%s\n-- generated\n%s\n", h, c->name,
                places[place], outcome[r], sizes[0], sizes[1], a, b);
}
/* the runtime library's functions a block calls, for the loader: 0, or -1 with the reason in error */
static SnxCodeLoader* new_loader(char* error, size_t size) {
    SnxCodeLoader* loader = snx_code_loader_new(0, error, size);
    if (loader && (snx_code_define(loader, "snx_arm_fp", (SnxU64)(uintptr_t)snx_arm_fp) ||
                   snx_code_define(loader, "snx_arm_fp_env", (SnxU64)(uintptr_t)snx_arm_fp_env) ||
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
/* the blocks of count halfwords, emitted into a module of their own by each emitter: printing one function walks its
   whole module, so a module of every halfword would make the comparison quadratic */
static int run_chunk(SnxIr* ir, const Config* c, int place, const uint32_t* halfwords, size_t count) {
    SnxCodeLoader* loader = NULL;
    LLVMValueRef* fn[2];
    LLVMModuleRef module[2];
    SnxU64* addr[2];
    char error[512];
    int rc = 0;
    for (int k = 0; k < 2; ++k) {
        fn[k] = (LLVMValueRef*)snx_realloc(NULL, sizeof(LLVMValueRef) * (count + 1));
        addr[k] = (SnxU64*)snx_realloc(NULL, sizeof(SnxU64) * (count + 1));
        module[k] = emit_all(ir, c, place, halfwords, count, k == 0, fn[k]);
        if (snx_ir_module_optimize(ir, module[k], '2', error, sizeof error)) {
            fprintf(stderr, "FAIL: %s %s: %s\n", c->name, k ? "generated" : "hand-written", error);
            ++g_test.failures;
            rc = -1;
        }
    }
    for (size_t i = 0; i < count && !rc; ++i) {
        char *a, *b;
        size_t sizes[2];
        if (!fn[0][i] || !fn[1][i]) {
            fprintf(stderr, "FAIL: %04x %s (%s): the %s emitter refused its block\n", halfwords[i], c->name,
                    places[place], fn[0][i] ? "generated" : "hand-written");
            ++g_test.failures;
            continue;
        }
        judge(compare_fns(fn[0][i], fn[1][i], &a, &b, sizes), c, place, halfwords[i], a, b, sizes);
        free(a);
        free(b);
    }
    if (!rc && c->run && g_test.states && !(loader = new_loader(error, sizeof error))) {
        fprintf(stderr, "FAIL: no code loader: %s\n", error);
        ++g_test.failures;
        rc = -1;
    }
    if (!rc && loader && load(ir, loader, module, count, addr)) {
        ++g_test.failures;
        rc = -1;
    }
    if (!rc && loader) {
        RunState* s = (RunState*)snx_realloc(NULL, 2 * sizeof *s);
        int legacy = c->profile == SNX_ARM_PROFILE_ARM7TDMI || c->profile == SNX_ARM_PROFILE_ARM946E;
        for (size_t i = 0; i < count; ++i) {
            uint16_t code[3];
            size_t n = block_of(halfwords[i], place, code);
            for (unsigned k = 0; k < g_test.states; ++k) {
                u64 seed = g_test.seed;
                const char* why;
                for (int side = 0; side < 2; ++side) {
                    g_test.seed = seed;
                    start(&s[side], k, code, n, legacy);
                    g_test.logged = 0;
                    ((SnxArmBlock)(uintptr_t)addr[side][i])(&s[side].cpu.regs);
                    finish(&s[side]);
                }
                ++g_test.ran;
                why = differs(&s[0], &s[1]);
                if (g_test.only)
                    printf(
                        "state %u: hand-written exit %u r15 %08x cpsr %08x, generated exit %u r15 %08x cpsr %08x%s%s\n",
                        k, s[0].cpu.regs.exit, s[0].cpu.regs.r[15], s[0].cpu.regs.cpsr, s[1].cpu.regs.exit,
                        s[1].cpu.regs.r[15], s[1].cpu.regs.cpsr, *why ? ": " : "", why);
                if (*why) {
                    ++g_test.failures;
                    fprintf(stderr, "FAIL: %04x %s (%s), state %u: %s\n", halfwords[i], c->name, places[place], k, why);
                    break;
                }
            }
        }
        free(s);
    }
    for (int k = 0; k < 2; ++k) {
        LLVMDisposeModule(module[k]);
        free(fn[k]);
        free(addr[k]);
    }
    if (loader) snx_code_loader_free(loader);
    return rc;
}
#define CHUNK 256
static int run_config(SnxIr* ir, const Config* c, int place, const uint32_t* halfwords, size_t count) {
    for (size_t at = 0; at < count; at += CHUNK)
        if (run_chunk(ir, c, place, halfwords + at, count - at < CHUNK ? count - at : CHUNK)) return -1;
    return 0;
}

/* ---- the witnesses: what the architecture says of the forms the emitters once ran otherwise ----
   BKPT runs whatever an IT block's condition, an SVC its condition skips lets the block go on, an UNPREDICTABLE
   encoding runs as it is written (arm.sail's UnpredictableEncoding) but an empty LDM, which is UNDEFINED: each block on
   the A57, flat, from one state, checked against its answer with both emitters */
typedef struct {
    const char* what;
    uint16_t code[3];
    size_t count;
    uint32_t flags, r0, r1; /* NZCV, and r0 and r1 as it starts */
    uint32_t exit, r15;     /* as it ends */
    uint32_t nzcv;          /* as it ends, or ~0u where the flags do not matter */
} Witness;
static const Witness witnesses[] = {
    {"BKPT in an IT block whose condition fails", {0xBF08, 0xBE12}, 2, 0, 0, 0, SNX_ARM_BREAKPOINT, 0x1004, ~0u},
    {"B<c> in an IT block, under its own condition", {0xBFC8, 0xD000}, 2, 0x40000000u, 0, 0, SNX_ARM_OK, 0x1006, ~0u},
    {"CBZ in an IT block whose condition fails", {0xBF08, 0xB100}, 2, 0, 0, 0, SNX_ARM_OK, 0x1006, ~0u},
    {"LSLS #0 (MOVS) in an IT block sets the flags",
     {0xBF08, 0x0008},
     2,
     0x40000000u,
     0,
     0x80000000u,
     SNX_ARM_OK,
     0x1004,
     0x80000000u},
    {"an empty LDM is UNDEFINED", {0xC800}, 1, 0, 0x2000, 0, SNX_ARM_UNDEFINED, 0x1000, ~0u},
    {"CMP of two low registers in the high form", {0x4508}, 1, 0, 1, 2, SNX_ARM_OK, 0x1002, 0x80000000u},
    {"SVC skipped in an IT block, the block going on", {0xBF04, 0xDF01, 0x4608}, 3, 0, 0, 0, SNX_ARM_OK, 0x1006, ~0u},
};
#define WITNESSES (sizeof witnesses / sizeof *witnesses)
static void run_witnesses(SnxIr* ir) {
    const Config* c = &configs[0];
    SnxArmCodegenOptions o = options_of(c);
    SnxCodeLoader* loader;
    LLVMModuleRef module[2];
    SnxU64 addr[2][WITNESSES + 1];
    SnxU64* addrs[2] = {addr[0], addr[1]};
    RunState* s = (RunState*)snx_realloc(NULL, sizeof *s);
    char error[512];
    for (int k = 0; k < 2; ++k) {
        module[k] = LLVMModuleCreateWithNameInContext(k ? "sail" : "hand", ir->ctx);
        LLVMSetTarget(module[k], LLVMGetTarget(ir->mod));
        LLVMSetDataLayout(module[k], LLVMGetDataLayoutStr(ir->mod));
        for (size_t i = 0; i < WITNESSES; ++i) {
            char name[32];
            size_t consumed;
            snprintf(name, sizeof name, "T_%zu", i);
            if (!(k ? snx_thumb_llvm_block_ex : snx_thumb_llvm_block_t16_reference)(
                    module[k], name, witnesses[i].code, witnesses[i].count, PC, &consumed, &o)) {
                fprintf(stderr, "FAIL: witness %s: the %s emitter refused it\n", witnesses[i].what,
                        k ? "generated" : "hand-written");
                ++g_test.failures;
            }
        }
        if (snx_ir_module_optimize(ir, module[k], '2', error, sizeof error)) {
            fprintf(stderr, "FAIL: witnesses: %s\n", error);
            ++g_test.failures;
        }
    }
    loader = g_test.failures ? NULL : new_loader(error, sizeof error);
    if (loader && load(ir, loader, module, WITNESSES, addrs)) {
        snx_code_loader_free(loader);
        loader = NULL;
    }
    if (!loader) {
        fprintf(stderr, "FAIL: the witnesses do not run\n");
        ++g_test.failures;
    }
    for (size_t i = 0; loader && i < WITNESSES; ++i)
        for (int k = 0; k < 2; ++k) {
            const Witness* w = &witnesses[i];
            SnxArmCPU* cpu = &s->cpu.regs;
            start(s, 0, w->code, w->count, 0);
            cpu->r[0] = w->r0;
            cpu->r[1] = w->r1;
            cpu->cpsr = 0x10u | SNX_ARM_T | w->flags;
            ((SnxArmBlock)(uintptr_t)addr[k][i])(cpu);
            if (cpu->exit != w->exit || cpu->r[15] != w->r15 ||
                (w->nzcv != ~0u && (cpu->cpsr & 0xF0000000u) != w->nzcv)) {
                fprintf(stderr,
                        "FAIL: witness %s, %s: exit %u r15 %08x NZCV %x, the architecture's exit %u r15 %08x%s\n",
                        w->what, k ? "generated" : "hand-written", cpu->exit, cpu->r[15], cpu->cpsr >> 28, w->exit,
                        w->r15, w->nzcv != ~0u ? " and its NZCV" : "");
                ++g_test.failures;
            }
            finish(s);
        }
    for (int k = 0; k < 2; ++k) LLVMDisposeModule(module[k]);
    if (loader) snx_code_loader_free(loader);
    free(s);
    printf("%zu witnesses run with both emitters\n", WITNESSES);
}

/* --time ROUNDS: what each emitter costs the recompile, the halfwords alone on the A57 emitted and optimized (O2) in
   modules of CHUNK blocks, ROUNDS times with each, alternating, in processor time; each round's line, so a chart shows
   the noise (llvm_sail_ir.c's format), then the best round of each, not a gate */
static int time_emitters(SnxIr* ir, const uint32_t* halfwords, size_t count, unsigned rounds) {
    const Config* c = &configs[0];
    LLVMValueRef* fn = (LLVMValueRef*)snx_realloc(NULL, sizeof(LLVMValueRef) * (CHUNK + 1));
    double best[2] = {1e30, 1e30}, emit_best[2] = {1e30, 1e30};
    char error[512];
    if (!snx_process_cpu_ns()) {
        free(fn);
        fprintf(stderr, "FAIL: --time: this system gives no processor time (snx_process_cpu_ns)\n");
        return 1;
    }
    for (unsigned r = 0; r < rounds; ++r)
        for (int k = 0; k < 2; ++k) {
            double emit = 0, total = 0;
            for (size_t at = 0; at < count; at += CHUNK) {
                double t0 = (double)snx_process_cpu_ns() / 1e9, t1, t2;
                LLVMModuleRef module =
                    emit_all(ir, c, ALONE, halfwords + at, count - at < CHUNK ? count - at : CHUNK, k == 0, fn);
                t1 = (double)snx_process_cpu_ns() / 1e9;
                if (snx_ir_module_optimize(ir, module, '2', error, sizeof error)) {
                    fprintf(stderr, "FAIL: --time: %s\n", error);
                    LLVMDisposeModule(module);
                    free(fn);
                    return 1;
                }
                t2 = (double)snx_process_cpu_ns() / 1e9;
                LLVMDisposeModule(module);
                emit += t1 - t0;
                total += t2 - t0;
            }
            printf("round %u, %s: %.4f s (emit %.4f s)\n", r + 1, k ? "generated" : "hand-written", total, emit);
            if (total < best[k]) best[k] = total;
            if (emit < emit_best[k]) emit_best[k] = emit;
        }
    printf(
        "time, best of %u, %zu halfwords: hand-written %.3f s (emit %.3f s), generated %.3f s (emit %.3f s): %+.1f%% "
        "(emit %+.1f%%)\n",
        rounds, count, best[0], emit_best[0], best[1], emit_best[1], 100.0 * (best[1] - best[0]) / best[0],
        100.0 * (emit_best[1] - emit_best[0]) / emit_best[0]);
    free(fn);
    return 0;
}

/* words the whole corpus found longer and a sample may miss: an ADD, an RSC and an ORR writing the PC with bit 0 the
   condition or the PC makes 0 (their CPSR was stored as it was); two RSCs, whose NOT(R[n]) goes second in AddWithCarry
   as llvm.c's reverse subtraction adds it */
static const uint32_t a32_found[] = {0x208ffb03u, 0x22effab8u, 0x618ffb08u, 0x30f72e0fu, 0x52fb01feu};
/* --a32's ith word of 65536: data processing (immediate, register, register-shifted), B, BL, BLX, BX and MOVW/MOVT,
   the class and the fields drawn from i, a third of them under AL and the rest under any condition, a register field
   15 one time in eight */
static uint32_t a32_word(uint32_t i) {
    uint64_t x = (uint64_t)i * 0x9E3779B97F4A7C15ull;
    uint32_t r, w, cond;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    r = (uint32_t)x;
    cond = (x >> 40) % 3 == 0 ? 14u : (uint32_t)(x >> 44) & 15;
    switch ((x >> 48) % 8) {
        case 0: w = 0x02000000u | (r & 0x01ffffffu); break; /* data processing, immediate */
        case 1:
        case 2: w = (r & 0x01ffff6fu); break;                                 /* register, shifted by an immediate */
        case 3: w = 0x00000010u | (r & 0x01ffff6fu); break;                   /* register, shifted by a register */
        case 4: w = 0x0a000000u | (r & 0x01ffffffu); break;                   /* B, BL */
        case 5: w = 0x012fff10u | (r & 0x2fu); break;                         /* BX, BLX */
        case 6: w = 0x03000000u | (r & 0x004fffffu); break;                   /* MOVW, MOVT */
        default: w = (r & 1 ? 0xfa000000u : 0x02000000u) | (r & 0x01ffffffu); /* BLX (immediate) or immediate */
    }
    for (unsigned f = 0; f < 4; ++f) /* Rm, Rs, Rd, Rn */
        if (((x >> (52 + 3 * f)) & 7) == 0) w |= 15u << (4 * f == 4 ? 8 : 4 * f == 8 ? 12 : 4 * f == 12 ? 16 : 0);
    return (w >> 28) == 15 ? w : (w & 0x0fffffffu) | cond << 28;
}

int main(int argc, char** argv) {
    char error[512];
    uint32_t* halfwords =
        (uint32_t*)snx_realloc(NULL, sizeof(uint32_t) * (0x10000 + sizeof a32_found / sizeof *a32_found));
    size_t count = 0, thumb2 = 0;
    SnxIr* ir;
    g_test.states = 8;
    g_test.part = 1;
    g_test.parts = 1;
    g_test.every = 1;
    g_test.seed = 0x5a11u;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--report") && i + 1 < argc) {
            g_test.report = fopen(argv[++i], "w");
            if (!g_test.report) return fprintf(stderr, "cannot write %s\n", argv[i]), 2;
        } else if (!strcmp(argv[i], "--states") && i + 1 < argc)
            g_test.states = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc)
            g_test.every = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--a32"))
            g_test.a32 = 1;
        else if (!strcmp(argv[i], "--word") && i + 1 < argc)
            g_test.word = (uint32_t)strtoul(argv[++i], NULL, 16), g_test.only = 1;
        else if (!strcmp(argv[i], "--time") && i + 1 < argc)
            g_test.time_rounds = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--part") && i + 1 < argc &&
                 sscanf(argv[++i], "%u/%u", &g_test.part, &g_test.parts) == 2 && g_test.part >= 1 &&
                 g_test.part <= g_test.parts)
            ;
        else
            return fprintf(stderr,
                           "usage: snx-test-arm-sail-ir [--a32] [--word HEX] [--report FILE] [--states N] [--part K/N] "
                           "[--every N] [--time ROUNDS]\n%s is not one of these\n",
                           argv[i]),
                   2;
    }
    if (!g_test.every) g_test.every = 1;
    /* the share of the halfwords: the Kth stretch of a walk that visits each once (an odd multiplier). Consecutive
       steps of it run through every value of the low bits, so a stretch holds every register and immediate; every
       Nth step would keep the low bits fixed. In their order, the T32 halves last */
    size_t share = (0x10000 + g_test.parts * g_test.every - 1) / (g_test.parts * g_test.every);
    for (uint32_t i = (uint32_t)((g_test.part - 1) * share); i < 0x10000 && i < g_test.part * share; ++i)
        halfwords[count++] = g_test.a32 ? a32_word(i) : (uint16_t)(i * 0x9E37u);
    if (g_test.a32 && g_test.part == 1) /* the words the whole corpus once found longer, in every first part */
        for (size_t i = 0; i < sizeof a32_found / sizeof *a32_found; ++i) halfwords[count++] = a32_found[i];
    if (g_test.only) {
        halfwords[0] = g_test.word;
        count = 1;
    }
    if (sample_spread(halfwords, count)) {
        free(halfwords);
        return 1;
    }
    qsort(halfwords, count, sizeof *halfwords, cmp_u32);
    if (g_test.a32)
        thumb2 = count;
    else
        while (thumb2 < count && halfwords[thumb2] < 0xE800) ++thumb2;
    if (snx_ir_init(error, sizeof error) || !(ir = snx_ir_new_jit("arm-sail-ir", '2', error, sizeof error))) {
        fprintf(stderr, "FAIL: no LLVM for this host: %s\n", error);
        return 1;
    }
    if (g_test.time_rounds) {
        int r = time_emitters(ir, halfwords, thumb2, g_test.time_rounds);
        snx_ir_free(ir);
        free(halfwords);
        return r;
    }
    if (!g_test.a32) run_witnesses(ir);
    for (size_t c = 0; c < sizeof configs / sizeof *configs; ++c)
        for (int place = 0; place < PLACES; ++place) {
            if (place != ALONE && (!configs[c].it || g_test.a32)) continue;
            if (run_config(ir, &configs[c], place, halfwords, configs[c].it ? thumb2 : count)) break;
        }
    printf("%zu halfwords (part %u/%u, every %u): %zu blocks compared: %zu the same, %zu reordered, %zu another IR not "
           "longer, %zu longer; %zu runs\n",
           count, g_test.part, g_test.parts, g_test.every, g_test.compared, g_test.outcomes[SAME],
           g_test.outcomes[REORDERED], g_test.outcomes[OTHER_NOT_LONGER], g_test.outcomes[LONGER], g_test.ran);
    if (g_test.report) fclose(g_test.report);
    snx_ir_free(ir);
    free(halfwords);
    if (g_test.failures) printf("%zu failures\n", g_test.failures);
    return g_test.failures != 0;
}
