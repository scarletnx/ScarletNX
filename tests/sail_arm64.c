/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's arm64 back end (tools/sailgen/arm64.h, the A64 Sail's encoders) against its LLVM back end, word by word,
   as sail_x64.c holds the x86-64 one: every word of the decoder's rows (llvm_qemu_words.c, decoded for the ounce
   target, each again with Rd, Rn and Rm 31) that the roots (a64_x64.roots) take becomes arm64 code for u64
   step(CPU*), and an LLVM step; both run from the same random states, and every register, the flags, the SIMD&FP
   state, guest memory, the next pc and the runtime's UNDEF must match. Then sequences of two to four of those words
   become one block each and run beside the LLVM steps one after another.

   The arm64 code runs under Unicorn (QEMU's AArch64, its "max" CPU) on any host: the test's own memory (the code, the
   CPU structure, guest memory, a stack) is mapped into it at the same addresses, so the code and this process's C
   share every pointer, and a call out (a helper by its name, the runtime's RtAPI functions) goes to a page of traps
   whose hook calls the native function with the registers' arguments.
     snx-test-sail-arm64 [--states N] [--samples N] [--sequences N] [--pool N] [--verbose] [--dump WORD...]
   --dump prints the words' block as hex bytes for a disassembler (llvm-mc --disassemble -triple=aarch64) */
#include "sail_arm64_emit.h"
#include "llvm_steps.h"
#include "llvm_qemu_words.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unicorn/unicorn.h>

#define TARGET SNX_A64_TARGET_OUNCE
#define CODE_MAX 4096
#define MEMORY_SIZE    (4u << 20)
#define MEMORY_ZERO    (2u << 20)
#define MEMORY_TOUCHED 0x14000u
#define PAGE 4096u
#define STACK_SIZE (64u << 10)
#define HOST (SNX_ARM64_LSE | SNX_ARM64_CRC32)

static struct {
    u64 seed;
    unsigned undef;
    int verbose;
    sigjmp_buf jump;
    int jumping;
    unsigned char* memory[3]; /* the LLVM side's, the arm64 side's, the bytes both start from */
    unsigned sev, wfe;
    uc_engine* uc;
    unsigned char* trap; /* a page of RET words: each 16 bytes a native function's entry, the last the exit */
    void* trap_fn[64];
    const char* trap_name[64];
    unsigned ntraps;
    RtAPI* rt;    /* the arm64 side's RtAPI, its functions traps */
    CPU* cpu;     /* the arm64 side's state, in mapped memory */
    unsigned char* stack;
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
    return snx_a64_decode_word(word, TARGET, &insn) != SNX_A64_OP_UNDEF ? snx_a64_op_text(insn.op) : NULL;
}

static void on_abort(int signal_number) {
    (void)signal_number;
    if (g_test.jumping) siglongjmp(g_test.jump, 1);
}
static void on_sev(void) { ++g_test.sev; }
static void on_wfe(CPU* c) {
    (void)c;
    ++g_test.wfe;
}
static void on_undef(CPU* c, u64 pc, u32 word) {
    (void)c, (void)pc, (void)word;
    ++g_test.undef;
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

static u64 trap_of(void* f, const char* name) {
    unsigned k;
    for (k = 0; k < g_test.ntraps; ++k)
        if (g_test.trap_fn[k] == f) return (u64)(uintptr_t)(g_test.trap + 16 * k);
    if (g_test.ntraps == PAGE / 16 - 1) return 0;
    g_test.trap_fn[g_test.ntraps] = f;
    g_test.trap_name[g_test.ntraps] = name;
    return (u64)(uintptr_t)(g_test.trap + 16 * g_test.ntraps++);
}
#define TRAP_FN(name) {#name, (void*)name}
static u64 resolve(const char* sym) {
    static const struct {
        const char* name;
        void* f;
    } table[] = {TRAP_FN(snx_x64_fp_binary), TRAP_FN(snx_x64_fp_fma), TRAP_FN(snx_x64_fp_unary),
                 TRAP_FN(snx_x64_fp_minmax), TRAP_FN(snx_x64_fp_compare), TRAP_FN(snx_x64_fp_convert),
                 TRAP_FN(snx_x64_fp_round), TRAP_FN(snx_x64_fp_round_int), TRAP_FN(snx_x64_fp_to_int),
                 TRAP_FN(snx_x64_fp_from_int), TRAP_FN(snx_x64_fp_jcvt), TRAP_FN(snx_x64_fp_bfcvt)};
    size_t i;
    for (i = 0; i < sizeof table / sizeof *table; ++i)
        if (!strcmp(sym, table[i].name)) return trap_of(table[i].f, table[i].name);
    return 0;
}
/* the native function of a trap the code reached, called with X0 to X5 (System V passes them all the same way, a
   function of fewer ignoring the rest), its result into X0; the trap's RET goes back */
typedef u64 (*Native)(u64, u64, u64, u64, u64, u64);
static void on_trap(uc_engine* uc, uint64_t address, uint32_t size, void* user) {
    unsigned k = (unsigned)((address - (u64)(uintptr_t)g_test.trap) / 16);
    u64 a[6], r;
    int regs[6] = {UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5};
    unsigned i;
    (void)size, (void)user;
    if (k >= g_test.ntraps) return; /* the exit's, which emu_start stops at */
    for (i = 0; i < 6; ++i) uc_reg_read(uc, regs[i], &a[i]);
    r = ((Native)g_test.trap_fn[k])(a[0], a[1], a[2], a[3], a[4], a[5]);
    uc_reg_write(uc, UC_ARM64_REG_X0, &r);
}

static int map(void* p, size_t size) {
    uc_err e = uc_mem_map_ptr(g_test.uc, (u64)(uintptr_t)p, (size + PAGE - 1) & ~(size_t)(PAGE - 1), UC_PROT_ALL, p);
    if (e != UC_ERR_OK) {
        fprintf(stderr, "FAIL: Unicorn maps no %p: %s\n", p, uc_strerror(e));
        return -1;
    }
    return 0;
}
static void* page_alloc(size_t size) {
    void* p = mmap(NULL, (size + PAGE - 1) & ~(size_t)(PAGE - 1), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

/* Unicorn with the test's memory mapped: the code's region, the state, guest memory, the traps, the stack */
static int unicorn_begin(unsigned char* code, size_t code_cap) {
    uc_hook hook;
    uc_err e = uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &g_test.uc);
    unsigned k;
    if (e != UC_ERR_OK) {
        fprintf(stderr, "FAIL: no Unicorn AArch64: %s\n", uc_strerror(e));
        return -1;
    }
    uc_ctl_set_cpu_model(g_test.uc, UC_CPU_ARM64_MAX);
    g_test.trap = (unsigned char*)page_alloc(PAGE);
    g_test.rt = (RtAPI*)page_alloc(sizeof(RtAPI));
    g_test.cpu = (CPU*)page_alloc(sizeof(CPU));
    g_test.stack = (unsigned char*)page_alloc(STACK_SIZE);
    if (!g_test.trap || !g_test.rt || !g_test.cpu || !g_test.stack) return -1;
    for (k = 0; k < PAGE / 4; ++k) { /* RET */
        g_test.trap[4 * k] = 0xC0, g_test.trap[4 * k + 1] = 0x03, g_test.trap[4 * k + 2] = 0x5F;
        g_test.trap[4 * k + 3] = 0xD6;
    }
    memset(g_test.rt, 0, sizeof *g_test.rt);
    g_test.rt->undef = (void (*)(CPU*, u64, u32))(uintptr_t)trap_of((void*)on_undef, "undef");
    g_test.rt->sev = (void (*)(void))(uintptr_t)trap_of((void*)on_sev, "sev");
    g_test.rt->wfe = (void (*)(CPU*))(uintptr_t)trap_of((void*)on_wfe, "wfe");
    if (map(code, code_cap) || map(g_test.trap, PAGE) || map(g_test.rt, sizeof(RtAPI)) || map(g_test.cpu, sizeof(CPU)) ||
        map(g_test.stack, STACK_SIZE) || map(g_test.memory[1], MEMORY_SIZE))
        return -1;
    e = uc_hook_add(g_test.uc, &hook, UC_HOOK_CODE, (void*)on_trap, NULL, (u64)(uintptr_t)g_test.trap,
                    (u64)(uintptr_t)g_test.trap + PAGE - 1);
    if (e != UC_ERR_OK) {
        fprintf(stderr, "FAIL: no hook of the traps: %s\n", uc_strerror(e));
        return -1;
    }
    return 0;
}
/* the code at code run on *c (copied into the mapped state and back): the next pc it returns */
static u64 run(unsigned char* code, CPU* c) {
    u64 x0 = (u64)(uintptr_t)g_test.cpu, sp = (u64)(uintptr_t)(g_test.stack + STACK_SIZE),
        lr = (u64)(uintptr_t)(g_test.trap + PAGE - 16), r = 0;
    uc_err e;
    *g_test.cpu = *c;
    g_test.cpu->rt = g_test.rt;
    uc_reg_write(g_test.uc, UC_ARM64_REG_X0, &x0);
    uc_reg_write(g_test.uc, UC_ARM64_REG_SP, &sp);
    uc_reg_write(g_test.uc, UC_ARM64_REG_X30, &lr);
    e = uc_emu_start(g_test.uc, (u64)(uintptr_t)code, lr, 0, 1u << 20);
    if (e != UC_ERR_OK) {
        u64 pc;
        uc_reg_read(g_test.uc, UC_ARM64_REG_PC, &pc);
        fprintf(stderr, "  Unicorn stopped at %llx (code %p): %s\n", (unsigned long long)pc, (void*)code, uc_strerror(e));
        r = ~0ull;
    } else
        uc_reg_read(g_test.uc, UC_ARM64_REG_X0, &r);
    g_test.cpu->rt = c->rt;
    *c = *g_test.cpu;
    return r;
}

static int is_ldst(u32 word) {
    const char* n = op_name(word);
    return n && (!strncmp(n, "LD", 2) || !strncmp(n, "ST", 2) || !strncmp(n, "CAS", 3) || !strncmp(n, "SWP", 3) ||
                 !strncmp(n, "PRF", 3));
}
static void ldst_start(CPU* c, u32 word, unsigned s) {
    SnxA64Insn insn;
    u64 address;
    snx_a64_decode_word(word, TARGET, &insn);
    c->x[31] = 0x1000 + (rnd() & 0xff0);
    c->x[insn.args.rn] = 0x1000 + (rnd() & 0xff0);
    c->x[insn.args.rm] = 0x1000 + (rnd() & 0xff0);
    if (insn.op == SNX_A64_OP_LDAPR_i || insn.op == SNX_A64_OP_STLR_i)
        c->x[insn.args.rn] -= (c->x[insn.args.rn] + (u64)(int64_t)insn.args.imm) & ((1u << insn.args.sz) - 1);
    address = c->x[insn.args.rn];
    c->excl_on = 0;
    if (!(s & 1)) return;
    if (insn.op == SNX_A64_OP_STXR || (insn.op == SNX_A64_OP_STXP && insn.args.sz == 3)) {
        c->excl_on = 1;
        c->excl_addr = address;
        memcpy(&c->excl_val, g_test.memory[2] + MEMORY_ZERO + address, 8);
        memcpy(&c->excl_val2, g_test.memory[2] + MEMORY_ZERO + address + 8, 8);
        if (s & 2) c->excl_val = ~c->excl_val;
        c->excl_size = insn.op == SNX_A64_OP_STXP ? 0x13 : (u32)insn.args.sz;
    }
}

static void report(const CPU* a, const CPU* b, const CPU* start, const u64* next, const unsigned* undef) {
    unsigned r;
    for (r = 0; r < 32; ++r)
        if (a->x[r] != b->x[r])
            fprintf(stderr, "  x%u: llvm %016llx arm64 %016llx (was %016llx)\n", r, (unsigned long long)a->x[r],
                    (unsigned long long)b->x[r], (unsigned long long)start->x[r]);
    fprintf(stderr, "  nzcv: llvm %u%u%u%u arm64 %u%u%u%u; next: llvm %llx arm64 %llx; undef %u %u\n", a->n, a->z,
            a->cf, a->vf, b->n, b->z, b->cf, b->vf, (unsigned long long)next[0], (unsigned long long)next[1], undef[0],
            undef[1]);
    for (r = 0; r < 32; ++r)
        if (memcmp(&a->v[r], &b->v[r], sizeof a->v[r]))
            fprintf(stderr, "  v%u: llvm %016llx:%016llx arm64 %016llx:%016llx\n", r, (unsigned long long)a->v[r].d[1],
                    (unsigned long long)a->v[r].d[0], (unsigned long long)b->v[r].d[1],
                    (unsigned long long)b->v[r].d[0]);
    if (a->fpsr != b->fpsr) fprintf(stderr, "  fpsr: llvm %08x arm64 %08x\n", a->fpsr, b->fpsr);
}

/* one start state of a run, s its number (which odd values it has) */
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
    start->pc = 0x1000;
}

int main(int argc, char** argv) {
    unsigned states = 64, samples = 3, sequences = 2000, i, k;
    u32 *rows = NULL, *words;
    size_t count, n = 0, failures = 0, unmade = 0, compared = 0, bytes = 0;
    unsigned char* code;
    size_t code_at = 0, code_cap = (size_t)1 << 26;
    size_t* offset;
    LlvmSteps set;
    char error[1024];
    RtAPI rt = {.undef = on_undef, .sev = on_sev, .wfe = on_wfe, .cas128 = snx_runtime_cas128};
    struct sigaction sa;
    snx_arm64_set_resolve(resolve);
    for (i = 1; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--states") && i + 1 < (unsigned)argc) states = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--samples") && i + 1 < (unsigned)argc)
            samples = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pool") && i + 1 < (unsigned)argc)
            snx_arm64_set_pool(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--sequences") && i + 1 < (unsigned)argc)
            sequences = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose"))
            g_test.verbose = 1;
        else if (!strcmp(argv[i], "--trace"))
            g_test.verbose = 2;
        else if (!strcmp(argv[i], "--dump") && i + 1 < (unsigned)argc) {
            unsigned char buf[CODE_MAX];
            u32 ws[16];
            size_t nw = 0, got, b;
            g_test.trap = (unsigned char*)page_alloc(PAGE);
            while (i + 1 < (unsigned)argc && nw < 16) ws[nw++] = (u32)strtoul(argv[++i], NULL, 16);
            got = snx_arm64_block(ws, &nw, 0x1000, TARGET, HOST, buf, sizeof buf);
            for (b = 0; b < got; ++b) printf("0x%02x%c", buf[b], b + 1 < got ? ' ' : '\n');
            if (!got) printf("(not made)\n");
            return 0;
        } else {
            fprintf(stderr, "usage: snx-test-sail-arm64 [--states N] [--samples N] [--sequences N] [--pool N] "
                            "[--verbose] [--dump WORD...]\n");
            return 2;
        }
    }
    g_test.seed = 0x9e3779b97f4a7c15ull;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_abort;
    sigaction(SIGABRT, &sa, NULL);
    for (k = 0; k < 3; ++k) {
        g_test.memory[k] = (unsigned char*)page_alloc(MEMORY_SIZE);
        if (!g_test.memory[k]) return 1;
    }
    for (i = 0; i < MEMORY_SIZE; ++i) g_test.memory[2][i] = (unsigned char)rnd();
    memcpy(g_test.memory[0], g_test.memory[2], MEMORY_SIZE);
    memcpy(g_test.memory[1], g_test.memory[2], MEMORY_SIZE);
    code = (unsigned char*)page_alloc(code_cap);
    if (!code || unicorn_begin(code, code_cap)) {
        fprintf(stderr, "FAIL: no memory or Unicorn for the code\n");
        return 1;
    }
    count = lqd_row_words(samples, rnd, &rows);
    words = (u32*)malloc(sizeof(u32) * (count * 8 + 1));
    offset = (size_t*)malloc(sizeof *offset * (count * 8 + 1));
    if (!words || !offset) return 1;
    for (i = 0; i < count; ++i) {
        static const u32 fields[3] = {0x1fu, 0x1fu << 5, 0x1fu << 16};
        const char* name = op_name(rows[i]);
        unsigned f;
        if (!name) continue;
        for (f = 0; f < 8; ++f) {
            u32 w = rows[i];
            size_t got = 0;
            int failed = 0;
            for (k = 0; k < 3; ++k)
                if (f & (1u << k)) w |= fields[k];
            if (f && (w == rows[i] || !op_name(w) || strcmp(op_name(w), name))) continue;
            g_test.jumping = 1;
            if (sigsetjmp(g_test.jump, 1)) failed = 1;
            else
                got = snx_arm64_step(w, 0x1000, TARGET, HOST, code + code_at, code_cap - code_at);
            g_test.jumping = 0;
            if (failed) {
                ++unmade;
                ++failures;
                fprintf(stderr, "FAIL: %s %08x: the arm64 back end takes it but can't make it\n", name, (unsigned)w);
                continue;
            }
            if (!got) continue;
            offset[n] = code_at;
            code_at += (got + 15) & ~(size_t)15;
            bytes += got;
            words[n++] = w;
        }
    }
    free(rows);
    if (llvm_steps_begin(&set, 0, LLVM_STEPS_HOST, error, sizeof error)) {
        fprintf(stderr, "FAIL: %s\n", error);
        return 1;
    }
    for (i = 0; i < n; ++i) llvm_steps_add(&set, words[i], 0x1000, TARGET);
    if (llvm_steps_load(&set, error, sizeof error)) {
        fprintf(stderr, "FAIL: %s\n", error);
        return 1;
    }
    for (i = 0; i < n; ++i) {
        unsigned s;
        for (s = 0; s < states; ++s) {
            CPU start, end[2];
            u64 next[2];
            unsigned undef[2];
            start_state(&start, s, &rt);
            if (is_ldst(words[i])) ldst_start(&start, words[i], s);
            end[0] = start;
            end[1] = start;
            end[0].mem = g_test.memory[0] + MEMORY_ZERO;
            end[1].mem = g_test.memory[1] + MEMORY_ZERO;
            g_test.undef = g_test.sev = g_test.wfe = 0;
            if (g_test.verbose == 2) fprintf(stderr, "run %s %08x state %u\n", op_name(words[i]), (unsigned)words[i], s);
            next[0] = set.steps[i](&end[0]);
            undef[0] = g_test.undef + 16 * g_test.sev + 256 * g_test.wfe;
            g_test.undef = g_test.sev = g_test.wfe = 0;
            next[1] = run(code + offset[i], &end[1]);
            undef[1] = g_test.undef + 16 * g_test.sev + 256 * g_test.wfe;
            ++compared;
            {
                int memory = memcmp(g_test.memory[0] + MEMORY_ZERO, g_test.memory[1] + MEMORY_ZERO, MEMORY_TOUCHED);
                end[0].mem = end[1].mem = NULL;
                if (memory || end[0].excl_on != end[1].excl_on) next[1] = ~next[0];
                memcpy(g_test.memory[0] + MEMORY_ZERO, g_test.memory[2] + MEMORY_ZERO, MEMORY_TOUCHED);
                memcpy(g_test.memory[1] + MEMORY_ZERO, g_test.memory[2] + MEMORY_ZERO, MEMORY_TOUCHED);
            }
            if (next[0] != next[1] || undef[0] != undef[1] || memcmp(end[0].x, end[1].x, sizeof end[0].x) ||
                end[0].n != end[1].n || end[0].z != end[1].z || end[0].cf != end[1].cf || end[0].vf != end[1].vf ||
                fp_differs(&end[0], &end[1])) {
                ++failures;
                fprintf(stderr, "FAIL: %s %08x state %u: the arm64 code's result differs\n", op_name(words[i]),
                        (unsigned)words[i], s);
                if (g_test.verbose) report(&end[0], &end[1], &start, next, undef);
                break;
            }
        }
    }
    llvm_steps_end(&set);
    if (n && sequences) {
        typedef struct {
            u32 w[4];
            size_t count, code, first;
        } Seq;
        Seq* seq = (Seq*)malloc(sizeof(Seq) * sequences);
        size_t q, nseq = 0, steps = 0;
        if (!seq) return 1;
        for (q = 0; q < sequences; ++q) {
            Seq* sq = &seq[nseq];
            size_t len = 2 + rnd() % 3, got = 0, j;
            int failed = 0;
            for (j = 0; j < len; ++j)
                do
                    sq->w[j] = words[rnd() % n];
                while (is_ldst(sq->w[j]));
            sq->count = len;
            g_test.jumping = 1;
            if (sigsetjmp(g_test.jump, 1)) failed = 1;
            else
                got = snx_arm64_block(sq->w, &sq->count, 0x1000, TARGET, HOST, code + code_at, code_cap - code_at);
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
            ++nseq;
        }
        uc_ctl_remove_cache(g_test.uc, (u64)(uintptr_t)code, (u64)(uintptr_t)code + code_cap);
        if (llvm_steps_begin(&set, 0, LLVM_STEPS_HOST, error, sizeof error)) {
            fprintf(stderr, "FAIL: %s\n", error);
            return 1;
        }
        for (q = 0; q < nseq; ++q) {
            size_t j;
            seq[q].first = steps;
            for (j = 0; j < seq[q].count; ++j, ++steps) llvm_steps_add(&set, seq[q].w[j], 0x1000 + 4 * j, TARGET);
        }
        if (llvm_steps_load(&set, error, sizeof error)) {
            fprintf(stderr, "FAIL: %s\n", error);
            return 1;
        }
        for (q = 0; q < nseq; ++q) {
            unsigned st;
            for (st = 0; st < states; ++st) {
                CPU start, end[2];
                u64 next[2] = {0, 0};
                unsigned undef[2];
                size_t j;
                start_state(&start, st & 3, &rt);
                end[0] = start;
                end[1] = start;
                g_test.undef = g_test.sev = g_test.wfe = 0;
                for (j = 0; j < seq[q].count; ++j) next[0] = set.steps[seq[q].first + j](&end[0]);
                undef[0] = g_test.undef + 16 * g_test.sev + 256 * g_test.wfe;
                g_test.undef = g_test.sev = g_test.wfe = 0;
                next[1] = run(code + seq[q].code, &end[1]);
                undef[1] = g_test.undef + 16 * g_test.sev + 256 * g_test.wfe;
                ++compared;
                if (next[0] != next[1] || undef[0] != undef[1] || memcmp(end[0].x, end[1].x, sizeof end[0].x) ||
                    end[0].n != end[1].n || end[0].z != end[1].z || end[0].cf != end[1].cf || end[0].vf != end[1].vf ||
                    fp_differs(&end[0], &end[1])) {
                    ++failures;
                    fprintf(stderr, "FAIL: the block of");
                    for (j = 0; j < seq[q].count; ++j)
                        fprintf(stderr, " %s %08x", op_name(seq[q].w[j]), (unsigned)seq[q].w[j]);
                    fprintf(stderr, " state %u: the arm64 code's result differs\n", st);
                    if (g_test.verbose) report(&end[0], &end[1], &start, next, undef);
                    break;
                }
            }
        }
        llvm_steps_end(&set);
        printf("sail_arm64: %zu blocks of 2 to 4 words\n", nseq);
        free(seq);
    }
    printf("sail_arm64: %zu words made (%zu bytes of arm64, %.1f a word), %zu runs, %zu failures (%zu words not "
           "made)\n",
           n, bytes, n ? (double)bytes / (double)n : 0.0, compared, failures, unmade);
    uc_close(g_test.uc);
    return failures ? 1 : 0;
}
