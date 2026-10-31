/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Seeded multi-block programs against QEMU: flag joins, bounded loops, guest calls, memory, exclusives and SIMD/FP.
   The self-tests plant a stale flag or a wrong exclusive-loop update only in the translated copy. */
#include "llvm_steps.h"
#include "qemu_ref/qemu_ref.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORDS 129
#define MEMORY 8192
#define ROOT_WORDS 20
#define HELPER_WORD 64
#define STOP_WORD 128
#define STATES 4

typedef struct {
    u32 words[WORDS];
    unsigned char image[WORDS * 4], root_leaders[ROOT_WORDS], helper_leaders[8];
    SnxCodeImage module;
    SnxFunction functions[2];
    SnxCodegen codegen;
    CPU initial[STATES];
} Program;
typedef struct { unsigned misses, undefined, polls; } Events;

static u64 next(u64* rng) {
    *rng ^= *rng << 13;
    *rng ^= *rng >> 7;
    *rng ^= *rng << 17;
    return *rng;
}
static void missed(CPU* c, u64 pc) {
    (void)pc;
    ((Events*)c->thread)->misses++;
    c->pc = 0;
}
static void undefined(CPU* c, u64 pc, u32 word) {
    (void)pc;
    (void)word;
    ((Events*)c->thread)->undefined++;
}
static void poll(CPU* c) {
    ((Events*)c->thread)->polls++;
    c->poll = 0;
}
static const RtAPI runtime = {.miss = missed, .undef = undefined, .poll = poll};

static SnxQrefState state_of(const CPU* c) {
    SnxQrefState s = {0};
    for (unsigned i = 0; i < 31; ++i) s.x[i] = c->x[i];
    s.sp = c->x[31];
    s.nzcv = c->n << 31 | c->z << 30 | c->cf << 29 | c->vf << 28;
    s.fpcr = c->fpcr;
    s.fpsr = c->fpsr;
    for (unsigned i = 0; i < 32; ++i)
        for (unsigned half = 0; half < 2; ++half) s.v[i][half] = snx_ld_le64(c->v[i].b + half * 8);
    return s;
}
static int same_state(const SnxQrefState* a, const SnxQrefState* b) {
    return !memcmp(a->x, b->x, sizeof a->x) && a->sp == b->sp && a->nzcv == b->nzcv &&
           a->fpcr == b->fpcr && a->fpsr == b->fpsr && !memcmp(a->v, b->v, sizeof a->v);
}

/* Base-A57 rows, with dependencies between randomly selected live registers. Never alter loop control or pointers. */
static u32 random_integer(u64* rng) {
    static const u32 rows[] = {0x8B000000u, 0xCB000000u, 0xCA000000u, 0x8A000000u, 0xAA000000u, 0x9B007C00u};
    static const unsigned regs[] = {0, 1, 2, 6, 7};
    unsigned op = (unsigned)(next(rng) % (sizeof rows / sizeof rows[0]));
    unsigned d = regs[next(rng) % 5], n = regs[next(rng) % 5], m = regs[next(rng) % 5];
    u32 word = rows[op] | m << 16 | n << 5 | d;
    if (op != 5) word |= (u32)(next(rng) & 7) << 10;
    return word;
}

static void generate(Program* p, unsigned index, u64* rng, int mutant, int hook) {
    u64 base = 0x100000ull + (u64)index * 0x1000;
    unsigned shift = index ? (unsigned)(next(rng) & 7) : 0;
    unsigned ca = index ? (unsigned)(next(rng) & 7) : 0;
    unsigned cb = index ? (unsigned)(next(rng) & 7) : 1;
    for (unsigned i = 0; i < WORDS; ++i) p->words[i] = SNX_QREF_PAD;
    const u32 root[ROOT_WORDS] = {
        0xAA1E03FCu, 0xD2800065u, 0x8B010000u, 0xCA000042u,
        0xF100001Fu | ca << 10, 0x54000080u, 0x8B020000u,
        0xF100003Fu | cb << 10, 0x54000041u, 0x9A800046u,
        0x94000000u | (HELPER_WORD - 10), 0x6E22DC20u, 0x4E22D403u,
        0xF9000460u, 0xF9400467u, 0xCA470042u | shift << 10,
        0xF10004A5u, 0x54000001u | ((unsigned)-15 & 0x7ffffu) << 5,
        0xAA1C03FEu, 0xD65F03C0u,
    };
    const u32 helper[] = {0x8B000021u, 0xCA020000u, 0xB1000442u, 0xD65F03C0u};
    int exclusive = index % 3 == 1 || (mutant == 2 && !index);
    memcpy(p->words, root, sizeof root);
    memcpy(p->words + HELPER_WORD, helper, sizeof helper);
    if (index) {
        const unsigned positions[] = {2, 3, 6, 15, HELPER_WORD, HELPER_WORD + 1};
        for (unsigned i = 0; i < sizeof positions / sizeof positions[0]; ++i)
            p->words[positions[i]] = random_integer(rng);
        static const u32 fp[] = {0x4E22D420u, 0x4EA2D420u, 0x6E22DC20u, 0x6E22FC20u};
        p->words[11] = fp[next(rng) % (sizeof fp / sizeof fp[0])];
        if (next(rng) & 1) {
            p->words[13] = root[14];
            p->words[14] = root[13];
        }
    }
    if (exclusive) {
        /* LDXR X8,[X3]; ADD X8,X8,X1; STXR W9,X8,[X3]; CBNZ W9,retry, followed by the helper's arithmetic.
           Some programs use the acquire/release forms. No other thread writes this aligned scratch word. */
        const u32 atomic[] = {0xC85F7C68u, 0x8B010108u, 0xC8097C68u, 0x35FFFFA9u,
                              0x8B000021u, 0xCA020000u, 0xB1000442u, 0xD65F03C0u};
        memcpy(p->words + HELPER_WORD, atomic, sizeof atomic);
        if (index & 2) {
            p->words[HELPER_WORD] |= 0x8000u;
            p->words[HELPER_WORD + 2] |= 0x8000u;
        }
    }
    for (unsigned i = 0; i < WORDS; ++i) snx_st_le32(p->image + i * 4, p->words[i]);
    /* The reference retains CMP. Only the translated copy sees the planted stale-flag defect. */
    if (mutant && !index)
        snx_st_le32(p->image + (mutant == 2 ? HELPER_WORD + 1 : 7) * 4, 0xD503201Fu);
    const unsigned leaders[] = {0, 2, 6, 9, 10, 18};
    for (unsigned i = 0; i < sizeof leaders / sizeof leaders[0]; ++i) p->root_leaders[leaders[i]] = 1;
    p->helper_leaders[0] = 1;
    p->module.image = p->image;
    p->module.image_size = sizeof p->image;
    p->module.base = base;
    p->module.text_size = sizeof p->image;
    p->functions[0] = (SnxFunction){.start = base, .end = base + ROOT_WORDS * 4,
        .count = ROOT_WORDS, .leaders = p->root_leaders, .leader_count = sizeof leaders / sizeof leaders[0]};
    unsigned helper_count = exclusive ? 8 : 4;
    p->functions[1] = (SnxFunction){.start = base + HELPER_WORD * 4, .end = base + (HELPER_WORD + helper_count) * 4,
        .count = helper_count, .leaders = p->helper_leaders, .leader_count = 1};
    p->codegen = (SnxCodegen){.modules = &p->module, .module_count = 1,
        .functions = p->functions, .function_count = 2, .hook_direct_calls = hook};
    for (unsigned n = 0; n < STATES; ++n) {
        CPU* c = &p->initial[n];
        for (unsigned i = 0; i < 31; ++i) c->x[i] = next(rng);
        c->x[3] = 0x100 + (next(rng) & 63);
        if (exclusive) c->x[3] &= ~7ull;
        c->x[30] = base + STOP_WORD * 4;
        c->x[31] = 0x1800;
        c->pc = base;
        c->n = n & 1;
        c->z = (n >> 1) & 1;
        c->cf = 1;
        c->fpcr = (n & 3) << 22 | ((index >> 1) & 1) << 24 | (index & 1) << 25;
        c->fpsr = n & 1 ? 0x0800009fu : 0;
        for (unsigned v = 0; v < 32; ++v)
            for (unsigned half = 0; half < 2; ++half) snx_st_le64(c->v[v].b + half * 8, next(rng));
        if (!index && !n) { c->x[0] = 0; c->x[1] = 1; c->x[2] = 2; c->x[6] = 0; }
    }
}

static int run(const char* qemu, const char* runner, const char* dir, unsigned count, u64 seed,
               int jit, int portable, char opt, int hook, int mutant) {
    Program* programs = calloc(count, sizeof *programs);
    if (!programs) return snx_error("cannot allocate the block corpus");
    SnxQrefBatch batch = {0};
    LlvmSteps steps;
    char error[1024];
    if (llvm_steps_begin_opt(&steps, jit, portable, opt, error, sizeof error)) {
        free(programs);
        return snx_error("%s", error);
    }
    u64 rng = seed ? seed : 1;
    int failed = 0;
    for (unsigned i = 0; i < count; ++i) {
        Program* p = &programs[i];
        generate(p, i, &rng, mutant, hook);
        llvm_steps_add_function(&steps, &p->codegen, &p->functions[0]);
        llvm_steps_add_function(&steps, &p->codegen, &p->functions[1]);
        for (unsigned n = 0; n < STATES; ++n) {
            SnxQrefCase c = {.window_size = MEMORY, .seed_kind = SNX_QREF_SEED_RANDOM,
                .seed = seed, .code = p->module.base, .code_size = 0x1000, .pc = p->module.base,
                .flags = SNX_QREF_FULL_DIFF, .state = state_of(&p->initial[n])};
            if (snx_qref_add(&batch, &c, p->words, WORDS, NULL, 0) < 0) failed = 1;
        }
    }
    if (!failed && llvm_steps_load(&steps, error, sizeof error)) { fprintf(stderr, "%s\n", error); failed = 1; }
    if (!failed && snx_qref_run(&batch, qemu, runner, "cortex-a57", dir, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        failed = 1;
    }
    unsigned mismatches = 0;
    for (unsigned i = 0; i < count && !failed; ++i) {
        Program* p = &programs[i];
        GuestFn root, helper;
        memcpy(&root, &steps.steps[i * 2], sizeof root);
        memcpy(&helper, &steps.steps[i * 2 + 1], sizeof helper);
        GuestFn dispatch[WORDS] = {0};
        dispatch[0] = root;
        dispatch[HELPER_WORD] = helper;
        for (unsigned n = 0; n < STATES; ++n) {
            const SnxQrefResult* ref = &batch.results[i * STATES + n];
            union { u64 alignment; u8 bytes[MEMORY]; } memory;
            u8* actual = memory.bytes;
            u8 expected[MEMORY];
            for (unsigned at = 0; at < MEMORY; at += 8) {
                u64 value = snx_qref_seed64(SNX_QREF_SEED_RANDOM, seed, 0, at);
                snx_st_le64(actual + at, value);
                snx_st_le64(expected + at, value);
            }
            for (unsigned change = 0; change < ref->changes; ++change) {
                const SnxQrefChange* c = &batch.changes[ref->first_change + change];
                if (c->address > MEMORY || c->size > MEMORY - c->address) { failed = 1; break; }
                memcpy(expected + c->address, batch.bytes + c->offset, c->size);
            }
            if (failed) break;
            Events events = {0};
            CPU c = p->initial[n];
            c.mem = actual;
            c.code_lo = p->module.base;
            c.code_hi = p->module.base + sizeof p->image;
            c.disp = dispatch;
            c.rt = &runtime;
            c.thread = &events;
            c.poll = 1;
            root(&c, p->module.base);
            SnxQrefState end = state_of(&c);
            int match = ref->outcome == SNX_QREF_BRK && ref->pc == p->module.base + STOP_WORD * 4 &&
                        c.pc == ref->pc && !ref->truncated && !events.misses && !events.undefined && events.polls &&
                        !c.excl_on && same_state(&end, &ref->state) && !memcmp(actual, expected, MEMORY);
            if (!match) {
                ++mismatches;
                if (mismatches <= 3)
                    fprintf(stderr, "block seed %llx program %u state %u O%c jit%d portable%d hook%d: "
                                    "pc %llx/%llx x6 %llx/%llx outcome %u FPSR %x/%x\n",
                            (unsigned long long)seed, i, n, opt, jit, portable, hook,
                            (unsigned long long)c.pc, (unsigned long long)ref->pc,
                            (unsigned long long)c.x[6], (unsigned long long)ref->state.x[6], ref->outcome,
                            c.fpsr, ref->state.fpsr);
            }
        }
    }
    llvm_steps_end(&steps);
    snx_qref_free(&batch);
    free(programs);
    if (failed) return 1;
    if (mutant) {
        if (!mismatches) return snx_error("the planted %s defect was not detected", mutant == 2 ? "exclusive-loop" : "stale-flag");
        printf("Block fuzzer self-test detected the planted %s defect\n", mutant == 2 ? "exclusive-loop" : "stale-flag");
        return 0;
    }
    printf("Block fuzzer: %u programs, %u comparisons, seed %llx, O%c jit%d portable%d hook%d: %u mismatches\n",
           count, count * STATES, (unsigned long long)seed, opt, jit, portable, hook, mismatches);
    return mismatches != 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && !strcmp(argv[1], "--skip")) { printf("Skipped: %s\n", argv[2]); return 77; }
    if (argc < 4) return snx_error("usage: llvm_block_fuzz QEMU RUNNER DIR [--jit] [--portable] [--opt=0|2] "
                                  "[--hook] [--volatile] [--self-test|--self-test-exclusive] [--seed=N] [--programs=N]");
    int jit = 0, portable = 0, hook = 0, mutant = 0;
    char opt = '2';
    unsigned count = 32;
    u64 seed = 0x5eed;
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--jit")) jit = 1;
        else if (!strcmp(argv[i], "--portable")) portable = 1;
        else if (!strcmp(argv[i], "--hook")) hook = 1;
        else if (!strcmp(argv[i], "--volatile")) snx_ir_volatile_memory(1);
        else if (!strcmp(argv[i], "--self-test")) { mutant = 1; count = 1; }
        else if (!strcmp(argv[i], "--self-test-exclusive")) { mutant = 2; count = 1; }
        else if (!strncmp(argv[i], "--opt=", 6) && strlen(argv[i]) == 7 && (argv[i][6] == '0' || argv[i][6] == '2'))
            opt = argv[i][6];
        else if (!strncmp(argv[i], "--seed=", 7)) {
            char* end;
            seed = strtoull(argv[i] + 7, &end, 0);
            if (!argv[i][7] || *end) return snx_error("invalid seed");
        } else if (!strncmp(argv[i], "--programs=", 11)) {
            char* end;
            unsigned long value = strtoul(argv[i] + 11, &end, 10);
            if (!argv[i][11] || *end || !value || value > 256) return snx_error("programs must be 1 to 256");
            count = (unsigned)value;
        } else return snx_error("unknown option %s", argv[i]);
    }
    return run(argv[1], argv[2], argv[3], count, seed, jit, portable, opt, hook, mutant);
}
