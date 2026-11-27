/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A native back end of sailgen other than x86-64's (SAIL_MACH: arm64's, tools/sailgen/arm64.h over the A64 Sail's
   encoders; riscv64's, rv64.h and risc.h over LLVM's RISC-V records...) against QEMU, word by word, as sail_x64.c holds
   the x86-64 one: every word of the decoder's rows (native_qemu_words.c, decoded for the ounce target, each again with
   Rd, Rn and Rm 31) that the roots (a64_x64.roots) take becomes the machine's code for u64 step(CPU*), and runs from
   the states of the word's own seed beside qemu-aarch64 -cpu max (sail_qemu.h): every register, the flags, the SIMD&FP
   state, the guest memory it changed, the next pc and the runtime's UNDEF must be QEMU's. Then sequences of two to four
   of those words become one block each and run beside the code of each word alone at its pc, one after another.

   The machine's code runs under Unicorn (QEMU's CPU of it, support/host_unicorn.c) on any host: the test's own memory
   (the code, the CPU structure, guest memory) is mapped into it at the same addresses, so the code and this
   process's C share every pointer, and a call out (a helper by its name, the runtime's RtAPI functions) goes to a page
   of traps whose hook calls the native function with the registers' arguments.
     snx-test-sail-MACH (--qemu QEMU RUNNER DIR | --skip REASON) [--frozen FILE] [--states N] [--samples N]
                        [--sequences N] [--pool N] [--xmm N] [--target NAME] [--verbose] [--ops OP...]
                        [--without MASK] [--dump WORD...]
   --qemu: qemu-aarch64's path, the runner's (tests/qemu_ref) and a folder for their files
   --frozen: the words QEMU can't stand for, and a store-exclusive whose monitor is armed where the memory changed,
   held to what LLVM's steps made of them (sail_qemu.h)
   --target: the guest's features, ounce (the default) or max
   --ops: the words of these ops of the decoder alone, and no sequences
   --dump prints the words' block as hex bytes for a disassembler (llvm-mc --disassemble -triple=MACH) */
#include "sail_mach_emit.h"
#include "support/host_unicorn.h"
#define SNX_TEST_SWAP_CPU 1
#include "support/swap_state.h"
#include "sail_qemu.h"
#include "native_qemu_words.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define CODE_MAX (64u << 10) /* --dump's */
/* the sequences' guest memory, which none of their words reaches: guest address 0 MEMORY_ZERO bytes in */
#define MEMORY_SIZE    (4u << 20)
#define MEMORY_ZERO    (2u << 20)
#define PAGE 4096u
#if defined(SAIL_MACH_WIN64)
/* the x86-64 back end's code for Windows, run by this host itself (support/host_win64.c): its processor's features, less
   those --without names */
#include "common/cpu/recompiler/x64/x64_codegen.h"
#define SAIL_MACH_HOST snx_x64_features()
/* reserved encodings (ADD shifted by ROR, UBFM of bit 32 up in 32 bits, ADD extended by 5), whose code calls undef */
static const u32 g_reserved[] = {0x0BC00000u, 0x53008000u, 0x0B20F400u};
#endif
enum { RESERVED = 3 }; /* g_reserved's */

#define HOST SAIL_MACH_HOST /* the machine's features the code may use (its SGX_F_*) */
#define USAGE                                                                                                          \
    "usage: snx-test-sail-%s (--qemu QEMU RUNNER DIR | --skip REASON) [--frozen FILE] [--states N] [--samples N]\n"    \
    "       [--sequences N] [--pool N] [--xmm N] [--target NAME] [--verbose] [--ops OP...] [--without MASK]\n"         \
    "       [--dump WORD...]\n"

static struct {
    u64 seed;
    u64 target; /* the guest's features: --target's, the Ounce's unless it names another */
    int verbose;
    sigjmp_buf jump;
    int jumping;
    unsigned char* memory; /* the sequences' guest memory */
    HostUc* uc;
    RtAPI* rt;    /* the machine's side's RtAPI, its functions traps */
    CPU* cpu;     /* the machine's side's state, in mapped memory */
} g_test;

static u64 rnd(void) {
    g_test.seed ^= g_test.seed << 13;
    g_test.seed ^= g_test.seed >> 7;
    g_test.seed ^= g_test.seed << 17;
    return g_test.seed;
}

static u64 fp_value(int dbl) {
    static const u64 d[] = {0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0xc008000000000000ull,
                            0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull, 0x7ff4000000000000ull,
                            0x000fffffffffffffull, 0x8000000000000001ull, 0x7fefffffffffffffull, 0x0010000000000000ull,
                            0x41dfffffffc00000ull, 0xc3e0000000000000ull, 0x3fe0000000000000ull, 0x4059000000000000ull};
    static const u32 f[] = {0, 0x80000000u, 0x3f800000u, 0xc0400000u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
                            0x7fa00000u, 0x007fffffu, 0x80000001u, 0x7f7fffffu, 0x00800000u, 0x4effffffu, 0xdf000000u,
                            0x3f000000u, 0x42c80000u};
    u64 r = rnd();
    if (r & 0x100) return r >> 9;
    if (dbl) return d[r & 15];
    return (u64)f[r & 15] | (u64)f[(r >> 4) & 15] << 32;
}
static void fp_state(CPU* c, unsigned s) {
    unsigned r;
    for (r = 0; r < 32; ++r) {
        c->v[r].d[0] = fp_value(rnd() & 1);
        c->v[r].d[1] = fp_value(rnd() & 1);
    }
    c->fpcr = s & 1 ? (u32)rnd() & (3u << 22 | 1u << 24 | 1u << 25 | 1u << 26 | 1u << 19) : 0;
    c->fpsr = (u32)rnd() & 0x9f;
}
static int fp_differs(const CPU* a, const CPU* b) {
    return memcmp(a->v, b->v, sizeof a->v) || a->fpsr != b->fpsr || a->fpcr != b->fpcr;
}

static const char* op_name(u32 word) {
    SnxA64Insn insn;
    return snx_a64_decode_word(word, g_test.target, &insn) != SNX_A64_OP_UNDEF ? snx_a64_op_text(insn.op) : NULL;
}

static void on_abort(int signal_number) {
    (void)signal_number;
    if (g_test.jumping) siglongjmp(g_test.jump, 1);
}
/* ---- the traps: a helper's name to its entry, the hook that calls it ---- */

u64 snx_x64_fp_binary(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_fma(CPU* c, u64 add, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_unary(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_minmax(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_compare(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_convert(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_round(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_round_int(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_to_int(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_from_int(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_jcvt(CPU* c, u64 a, u64 fpcr);
u64 snx_x64_fp_bfcvt(CPU* c, u64 a, u64 fpcr);

u64 snx_x64_crc32(u64 acc, u64 value, u32 desc);
u64 snx_x64_aes(u64 x, u32 op);
u64 snx_x64_estimate(u64 a, u32 sqrt);
u64 snx_x64_half(u64 a, u64 fpcr);
u64 snx_x64_bfdot(u64 sum, u64 a, u64 b);
u64 snx_x64_sm4_box(u64 a);
u64 snx_x64_stxp(CPU* c, u64 a, u64 v1, u64 v2);
extern const u32 snx_x64_crc32_table[8 * 256], snx_x64_crc32c_table[256];
HostUcPair snx_x64_casp(CPU* c, u64 a, u64 e0, u64 e1, u64 d0, u64 d1);

#define TRAP_FN(name) {#name, (void*)name}
static u64 resolve(const char* sym) {
    static const struct {
        const char* name;
        void* f;
    } table[] = {TRAP_FN(snx_x64_fp_binary), TRAP_FN(snx_x64_fp_fma), TRAP_FN(snx_x64_fp_unary),
                 TRAP_FN(snx_x64_fp_minmax), TRAP_FN(snx_x64_fp_compare), TRAP_FN(snx_x64_fp_convert),
                 TRAP_FN(snx_x64_fp_round), TRAP_FN(snx_x64_fp_round_int), TRAP_FN(snx_x64_fp_to_int),
                 TRAP_FN(snx_x64_fp_from_int), TRAP_FN(snx_x64_fp_jcvt), TRAP_FN(snx_x64_fp_bfcvt),
                 TRAP_FN(snx_x64_crc32), TRAP_FN(snx_x64_stxp), TRAP_FN(snx_x64_aes), TRAP_FN(snx_x64_estimate),
                 TRAP_FN(snx_x64_half), TRAP_FN(snx_x64_bfdot), TRAP_FN(snx_x64_sm4_box)};
    size_t i;
    if (!g_test.uc) return 0x1000; /* --dump's: no Unicorn, an address of the right size */
    if (!strcmp(sym, "snx_x64_casp")) return hostuc_native_wide(g_test.uc, sym, (void*)snx_x64_casp);
#if defined(SAIL_MACH_WIN64)
    if (!strcmp(sym, "snx_x64_crc32_table")) return (u64)(uintptr_t)snx_x64_crc32_table;
    if (!strcmp(sym, "snx_x64_crc32c_table")) return (u64)(uintptr_t)snx_x64_crc32c_table;
#endif
    for (i = 0; i < sizeof table / sizeof *table; ++i)
        if (!strcmp(sym, table[i].name)) return hostuc_native(g_test.uc, table[i].name, table[i].f);
    return 0;
}
/* a big-endian machine's state and runtime table turned over around its runs (host_unicorn.h's swapper) */
static void swap_state(void* user) {
    (void)user;
    snx_test_swap_cpu(g_test.cpu);
    snx_test_swap_rt(g_test.rt);
}
/* Unicorn with the test's memory mapped: the code's region, the state, guest memory, the runtime's traps */
static int unicorn_begin(unsigned char* code, size_t code_cap) {
    g_test.rt = (RtAPI*)hostuc_alloc(g_test.uc, sizeof(RtAPI));
    g_test.cpu = (CPU*)hostuc_alloc(g_test.uc, sizeof(CPU));
    if (!g_test.rt || !g_test.cpu) return -1;
    hostuc_swapper(g_test.uc, swap_state, NULL);
    RtAPI calls = {.cas128 = snx_runtime_cas128};
    sail_rt(&calls); /* g_sail_calls counts what they are given */
    memset(g_test.rt, 0, sizeof *g_test.rt);
    g_test.rt->undef = (void (*)(CPU*, u64, u32))(uintptr_t)hostuc_native(g_test.uc, "undef", (void*)calls.undef);
    g_test.rt->svc = (void (*)(CPU*, u32))(uintptr_t)hostuc_native(g_test.uc, "svc", (void*)calls.svc);
    g_test.rt->brk = (void (*)(CPU*, u64, u32))(uintptr_t)hostuc_native(g_test.uc, "brk", (void*)calls.brk);
    g_test.rt->sev = (void (*)(void))(uintptr_t)hostuc_native(g_test.uc, "sev", (void*)calls.sev);
    g_test.rt->wfe = (void (*)(CPU*))(uintptr_t)hostuc_native(g_test.uc, "wfe", (void*)calls.wfe);
    g_test.rt->cas128 =
        (u32 (*)(u64*, u64, u64, u64, u64))(uintptr_t)hostuc_native(g_test.uc, "cas128", (void*)calls.cas128);
    return hostuc_map(g_test.uc, code, code_cap) || hostuc_map(g_test.uc, g_test.memory, MEMORY_SIZE) ? -1 : 0;
}
/* the code at code run on *c (copied into the mapped state and back): the next pc it returns */
static u64 run(unsigned char* code, CPU* c) {
    u64 r = 0;
    *g_test.cpu = *c;
    g_test.cpu->rt = g_test.rt;
    if (hostuc_call(g_test.uc, (u64)(uintptr_t)code, (u64)(uintptr_t)g_test.cpu, 0, &r)) r = ~0ull;
    g_test.cpu->rt = c->rt;
    *c = *g_test.cpu;
    return r;
}

static int is_ldst(u32 word) { return sail_is_ldst(word); }

static void report(const CPU* a, const CPU* b, const CPU* start, const u64* next, const unsigned* calls) {
    unsigned r;
    for (r = 0; r < 32; ++r)
        if (a->x[r] != b->x[r])
            fprintf(stderr, "  x%u: words %016llx block %016llx (was %016llx)\n", r, (unsigned long long)a->x[r],
                    (unsigned long long)b->x[r], (unsigned long long)start->x[r]);
    fprintf(stderr, "  nzcv: words %u%u%u%u block %u%u%u%u; next: words %llx block %llx; calls %u %u\n", a->n, a->z,
            a->cf, a->vf, b->n, b->z, b->cf, b->vf, (unsigned long long)next[0], (unsigned long long)next[1], calls[0],
            calls[1]);
    for (r = 0; r < 32; ++r)
        if (memcmp(&a->v[r], &b->v[r], sizeof a->v[r]))
            fprintf(stderr, "  v%u: words %016llx:%016llx block %016llx:%016llx\n", r, (unsigned long long)a->v[r].d[1],
                    (unsigned long long)a->v[r].d[0], (unsigned long long)b->v[r].d[1],
                    (unsigned long long)b->v[r].d[0]);
    if (a->fpsr != b->fpsr) fprintf(stderr, "  fpsr: words %08x block %08x\n", a->fpsr, b->fpsr);
}

/* one start state of a sequence, s its number (which odd values it has) */
static void start_state(CPU* start, unsigned s, RtAPI* rt) {
    unsigned r;
    memset(start, 0, sizeof *start);
    for (r = 0; r < 32; ++r) start->x[r] = rnd();
    fp_state(start, s);
    if (s & 1) start->x[rnd() & 31] = 0;
    if (s & 2) start->x[rnd() & 31] = 0x8000000000000000ull;
    if (s & 4) start->x[rnd() & 31] = ~0ull;
    if (s & 8) start->x[rnd() & 31] = 0xffffffffull;
    start->n = rnd() & 1;
    start->z = rnd() & 1;
    start->cf = rnd() & 1;
    start->vf = rnd() & 1;
    start->rt = rt;
    start->pc = SAIL_PC;
}

#define NONE ((size_t)-1)
/* a word: its code at SAIL_PC, and a store-exclusive's after its load-exclusive at SAIL_PAIR_PC */
typedef struct {
    u32 word;
    size_t code, pair[2];
} Word;
typedef struct {
    const Word* words;
    unsigned char* code;
} Made;

/* word's code at pc on code's next bytes: its offset, NONE where the back end makes none; *failed where it takes the
   word but can't make it */
static size_t make(u32 word, u64 pc, unsigned char* code, size_t* at, size_t cap, int* failed) {
    size_t got = 0, where = *at;
    g_test.jumping = 1;
    if (sigsetjmp(g_test.jump, 1))
        *failed = 1;
    else
        got = snx_mach_step(word, pc, g_test.target, HOST, code + where, cap - where);
    g_test.jumping = 0;
    if (*failed || !got) return NONE;
    *at += (got + 15) & ~(size_t)15;
    return where;
}
static int has_code(void* user, size_t i, unsigned form) {
    (void)form;
    return ((const Made*)user)->words[i].code != NONE;
}
/* the native end of word i's code in state s from start, its memory in work (mapped into Unicorn) */
static void run_native(void* user, size_t i, unsigned form, unsigned s, const CPU* start, u8* work, const u8* initial,
                       SailEnd* e) {
    const Word* w = &((const Made*)user)->words[i];
    unsigned char* code = ((const Made*)user)->code;
    CPU c = *start;
    u64 next, pc = SAIL_PC;
    const int pair = start->pc == SAIL_PAIR_PC;
    (void)form, (void)s;
    sail_window_start(work, initial, w->word, pair);
    c.mem = work - SAIL_WINDOW;
    memset(&g_sail_calls, 0, sizeof g_sail_calls);
    if (pair) {
        (void)run(code + w->pair[0], &c);
        pc = SAIL_PAIR_PC + 4;
        next = g_sail_calls.undef ? pc : run(code + w->pair[1], &c);
    } else
        next = run(code + w->code, &c);
    sail_native_end(e, &c, next, pc, work, initial);
}

int main(int argc, char** argv) {
    static const char* const forms[1] = {"the machine's"};
    char** qemu = NULL;
    const char* frozen = NULL;
    unsigned states = SAIL_STATES_MAX, samples = 3, sequences = 2000, i, k, nops = 0;
    char** ops = NULL;
    u32 *rows = NULL, *list;
    Word* words;
    size_t count, n = 0, failures = 0, unmade = 0, compared = 0;
    unsigned char* code;
    size_t code_at = 0, code_cap = (size_t)1 << 26;
    SailHold hold = {0};
    Made made;
    RtAPI rt = {.cas128 = snx_runtime_cas128};
    struct sigaction sa;
#if defined(SAIL_MACH_WIN64)
    snx_x64_set_win64(1);
#endif
    snx_mach_set_resolve(resolve);
    g_test.target = SNX_A64_TARGET_OUNCE;
    for (i = 1; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--states") && i + 1 < (unsigned)argc) states = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--samples") && i + 1 < (unsigned)argc)
            samples = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pool") && i + 1 < (unsigned)argc)
            snx_mach_set_pool(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--xmm") && i + 1 < (unsigned)argc)
            snx_mach_set_xpool(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--sequences") && i + 1 < (unsigned)argc)
            sequences = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose"))
            g_test.verbose = 1;
        else if (!strcmp(argv[i], "--trace"))
            g_test.verbose = 2;
#if defined(SAIL_MACH_WIN64)
        else if (!strcmp(argv[i], "--without") && i + 1 < (unsigned)argc)
            snx_x64_without((unsigned)strtoul(argv[++i], NULL, 0));
#endif
        else if (!strcmp(argv[i], "--target") && i + 1 < (unsigned)argc) {
            const char* name = argv[++i];
            if (strcmp(name, "ounce") && strcmp(name, "max")) {
                fprintf(stderr, "--target %s: the targets are ounce and max\n", name);
                return 2;
            }
            g_test.target = !strcmp(name, "max") ? snx_a64_features(SNX_A64_TARGET_MAX) : SNX_A64_TARGET_OUNCE;
        } else if (!strcmp(argv[i], "--qemu") && i + 3 < (unsigned)argc) {
            qemu = argv + i + 1;
            i += 3;
        } else if (!strcmp(argv[i], "--frozen") && i + 1 < (unsigned)argc)
            frozen = argv[++i];
        else if (!strcmp(argv[i], "--skip") && i + 1 < (unsigned)argc) {
            printf("sail_%s skipped: %s\n", hostuc_name(SAIL_MACH_UC), argv[i + 1]);
            return 77;
        } else if (!strcmp(argv[i], "--ops") && i + 1 < (unsigned)argc) {
            ops = argv + i + 1;
            while (i + 1 < (unsigned)argc && argv[i + 1][0] != '-') ++i, ++nops;
            sequences = 0;
        } else if (!strcmp(argv[i], "--dump") && i + 1 < (unsigned)argc) {
            static unsigned char buf[CODE_MAX];
            u32 ws[16];
            size_t nw = 0, got, b;
            while (i + 1 < (unsigned)argc && nw < 16) ws[nw++] = (u32)strtoul(argv[++i], NULL, 16);
            got = snx_mach_block(ws, &nw, SAIL_PC, g_test.target, HOST, buf, sizeof buf);
            for (b = 0; b < got; ++b) printf("0x%02x%c", buf[b], b + 1 < got ? ' ' : '\n');
            if (!got) printf("(not made)\n");
            return 0;
        } else {
            fprintf(stderr, "snx-test-sail-%s: '%s' is not an option\n" USAGE, hostuc_name(SAIL_MACH_UC), argv[i],
                    hostuc_name(SAIL_MACH_UC));
            return 2;
        }
    }
    if (!qemu) {
        fprintf(stderr, "snx-test-sail-%s: the words are held to QEMU: --qemu QEMU RUNNER DIR is not given\n" USAGE,
                hostuc_name(SAIL_MACH_UC), hostuc_name(SAIL_MACH_UC));
        return 2;
    }
    if (states > SAIL_STATES_MAX) {
        fprintf(stderr, "snx-test-sail-%s: --states %u: a word has %u at most\n", hostuc_name(SAIL_MACH_UC), states,
                SAIL_STATES_MAX);
        return 2;
    }
    sail_set_target(g_test.target);
    g_test.seed = 0x9e3779b97f4a7c15ull;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_abort;
    sigaction(SIGABRT, &sa, NULL);
    g_test.memory = (unsigned char*)hostuc_pages(MEMORY_SIZE);
    hold.work = (u8*)hostuc_pages(SAIL_WINDOW_SIZE);
    code = (unsigned char*)hostuc_pages(code_cap);
    if (!g_test.memory || !hold.work) return 1;
    for (i = 0; i < MEMORY_SIZE; ++i) g_test.memory[i] = (unsigned char)rnd();
    g_test.uc = hostuc_new(SAIL_MACH_UC, PAGE);
    if (!g_test.uc) {
        printf("SKIP: no Unicorn of %s: nothing to compare\n", hostuc_name(SAIL_MACH_UC));
        return 77;
    }
    if (!code || unicorn_begin(code, code_cap) || hostuc_map(g_test.uc, hold.work, SAIL_WINDOW_SIZE)) {
        fprintf(stderr, "FAIL: no memory or Unicorn for the code\n");
        return 1;
    }
    count = sail_candidates(samples, &rows);
    words = (Word*)malloc(sizeof *words * (count + RESERVED));
    list = (u32*)malloc(sizeof *list * (count + RESERVED));
    if (!words || !list) return 1;
    for (i = 0; i < count; ++i) {
        const char* name = op_name(rows[i]);
        for (k = 0; k < nops && strcmp(name, ops[k]); ++k) {}
        if (nops && k == nops) continue;
        {
            const u32 w = rows[i];
            Word* x = &words[n];
            int failed = 0;
            x->word = w;
            x->pair[0] = x->pair[1] = NONE;
            x->code = make(w, SAIL_PC, code, &code_at, code_cap, &failed);
            if (x->code != NONE && sail_pair(w, 1)) {
                x->pair[0] = make(sail_pair(w, 1), SAIL_PAIR_PC, code, &code_at, code_cap, &failed);
                x->pair[1] = make(w, SAIL_PAIR_PC + 4, code, &code_at, code_cap, &failed);
                if (x->pair[0] == NONE || x->pair[1] == NONE) x->code = NONE;
            }
            if (failed) {
                ++unmade;
                ++failures;
                fprintf(stderr, "FAIL: %s %08x: the %s back end takes it but can't make it\n", name, (unsigned)w,
                        hostuc_name(SAIL_MACH_UC));
                continue;
            }
            if (x->code == NONE) continue;
            list[n++] = w;
        }
    }
    free(rows);
    for (k = 0; k < nops; ++k) {
        for (i = 0; i < n && strcmp(op_name(list[i]), ops[k]); ++i) {}
        if (i < n) continue;
        ++failures;
        fprintf(stderr, "FAIL: --ops: the %s back end makes no word of %s\n", hostuc_name(SAIL_MACH_UC), ops[k]);
    }
#if defined(SAIL_MACH_WIN64)
    for (i = 0; i < RESERVED; ++i) {
        int failed = 0;
        Word* x = &words[n];
        x->word = g_reserved[i];
        x->pair[0] = x->pair[1] = NONE;
        x->code = make(g_reserved[i], SAIL_PC, code, &code_at, code_cap, &failed);
        if (x->code == NONE) {
            ++failures;
            fprintf(stderr, "FAIL: %08x: the %s back end doesn't make its UNDEFINED\n", (unsigned)g_reserved[i],
                    hostuc_name(SAIL_MACH_UC));
            continue;
        }
        list[n++] = g_reserved[i];
    }
#endif
    hostuc_changed(g_test.uc, code, code_cap);
    made.words = words;
    made.code = code;
    hold.qemu = qemu;
    hold.forms = forms;
    hold.nforms = 1;
    hold.verbose = g_test.verbose;
    hold.user = &made;
    hold.has = has_code;
    hold.run = run_native;
    {
        char name[64];
        snprintf(name, sizeof name, "sail_%s", hostuc_name(SAIL_MACH_UC));
        hold.name = name;
        failures += sail_hold(&hold, list, n, states, &compared);
        if (frozen) failures += sail_frozen(&hold, frozen, list, n, &compared);
    }
    /* sequences: words in a row as one block, against the code of each word alone at its pc, one after another */
    sail_rt(&rt);
    if (n && sequences) {
        typedef struct {
            u32 w[4];
            size_t count, code, alone[4];
        } Seq;
        Seq* seq = (Seq*)malloc(sizeof(Seq) * sequences);
        size_t q, nseq = 0;
        if (!seq) return 1;
        for (q = 0; q < sequences; ++q) {
            Seq* sq = &seq[nseq];
            size_t len = 2 + rnd() % 3, got = 0, j;
            int failed = 0;
            for (j = 0; j < len; ++j)
                do
                    sq->w[j] = list[rnd() % n];
                while (is_ldst(sq->w[j]));
            sq->count = len;
            g_test.jumping = 1;
            if (sigsetjmp(g_test.jump, 1)) failed = 1;
            else
                got =
                    snx_mach_block(sq->w, &sq->count, SAIL_PC, g_test.target, HOST, code + code_at, code_cap - code_at);
            g_test.jumping = 0;
            if (failed) {
                ++failures;
                fprintf(stderr, "FAIL: the block of");
                for (j = 0; j < len; ++j) fprintf(stderr, " %08x", (unsigned)sq->w[j]);
                fprintf(stderr, " can't be made\n");
                continue;
            }
            if (!got) continue;
            sq->code = code_at;
            code_at += (got + 15) & ~(size_t)15;
            for (j = 0; j < sq->count && !failed; ++j)
                sq->alone[j] = make(sq->w[j], SAIL_PC + 4 * j, code, &code_at, code_cap, &failed);
            for (j = 0; j < sq->count; ++j) failed |= sq->alone[j] == NONE;
            if (failed) {
                ++failures;
                fprintf(stderr, "FAIL: a word of the block of %08x... can't be made alone at its pc\n",
                        (unsigned)sq->w[0]);
                continue;
            }
            ++nseq;
        }
        hostuc_changed(g_test.uc, code, code_cap);
        for (q = 0; q < nseq; ++q) {
            unsigned st;
            for (st = 0; st < states; ++st) {
                CPU start, end[2];
                u64 next[2] = {0, 0}, args[2];
                unsigned calls[2];
                size_t j;
                start_state(&start, st & 3, &rt);
                start.mem = g_test.memory + MEMORY_ZERO;
                end[0] = start;
                end[1] = start;
                memset(&g_sail_calls, 0, sizeof g_sail_calls);
                for (j = 0; j < seq[q].count && !g_sail_calls.undef; ++j)
                    next[0] = run(code + seq[q].alone[j], &end[0]);
                calls[0] = g_sail_calls.undef + 16 * g_sail_calls.sev + 256 * g_sail_calls.wfe;
                args[0] = g_sail_calls.undef_args;
                memset(&g_sail_calls, 0, sizeof g_sail_calls);
                next[1] = run(code + seq[q].code, &end[1]);
                calls[1] = g_sail_calls.undef + 16 * g_sail_calls.sev + 256 * g_sail_calls.wfe;
                args[1] = g_sail_calls.undef_args;
                ++compared;
                if ((!calls[0] && next[0] != next[1]) || calls[0] != calls[1] || args[0] != args[1] ||
                    memcmp(end[0].x, end[1].x, sizeof end[0].x) || end[0].n != end[1].n || end[0].z != end[1].z ||
                    end[0].cf != end[1].cf || end[0].vf != end[1].vf || fp_differs(&end[0], &end[1])) {
                    ++failures;
                    fprintf(stderr, "FAIL: the block of");
                    for (j = 0; j < seq[q].count; ++j)
                        fprintf(stderr, " %s %08x", op_name(seq[q].w[j]), (unsigned)seq[q].w[j]);
                    fprintf(stderr, " state %u: the %s block's end is not the words' one after another\n", st,
                            hostuc_name(SAIL_MACH_UC));
                    if (g_test.verbose) report(&end[0], &end[1], &start, next, calls);
                    break;
                }
            }
        }
        printf("sail_%s: %zu blocks of 2 to 4 words\n", hostuc_name(SAIL_MACH_UC), nseq);
        free(seq);
    }
    printf("sail_%s: %zu words made, %zu runs, %zu failures (%zu words not made)\n", hostuc_name(SAIL_MACH_UC), n,
           compared, failures, unmade);
    hostuc_free(g_test.uc);
    free(words), free(list);
    return failures ? 1 : 0;
}
